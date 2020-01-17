#include "SylinderSystem.hpp"

#include "Util/EquatnHelper.hpp"
#include "Util/GeoUtil.hpp"
#include "Util/IOHelper.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

#include <mpi.h>
#include <omp.h>

SylinderSystem::SylinderSystem(const std::string &configFile, const std::string &posFile, int argc, char **argv) {
    initialize(SylinderConfig(configFile), posFile, argc, argv);
}

SylinderSystem::SylinderSystem(const SylinderConfig &runConfig_, const std::string &posFile, int argc, char **argv) {
    initialize(runConfig_, posFile, argc, argv);
}

void SylinderSystem::initialize(const SylinderConfig &runConfig_, const std::string &posFile, int argc, char **argv) {
    runConfig = runConfig_;
    stepCount = 0;
    snapID = 0; // the first snapshot starts from 0 in writeResult

    // set MPI
    int mpiflag;
    MPI_Initialized(&mpiflag);
    TEUCHOS_ASSERT(mpiflag);
    commRcp = getMPIWORLDTCOMM();

    // TRNG pool must be initialized after mpi is initialized
    rngPoolPtr = std::make_shared<TRngPool>(runConfig.rngSeed);
    constraintSolverPtr = std::make_shared<ConstraintSolver>();
    uniConstraintPtr = std::make_shared<ConstraintCollector>();
    biConstraintPtr = std::make_shared<ConstraintCollector>();

    dinfo.initialize(); // init DomainInfo
    setDomainInfo();

    sylinderContainer.initialize();
    sylinderContainer.setAverageTargetNumberOfSampleParticlePerProcess(200); // more sample for better balance

    if (IOHelper::fileExist(posFile)) {
        setInitialFromFile(posFile);
    } else {
        setInitialFromConfig();
    }

    // showOnScreenRank0(); // at this point all sylinders located on rank 0

    commRcp->barrier();
    decomposeDomain();
    exchangeSylinder(); // distribute to ranks, initial domain decomposition

    sylinderNearDataDirectoryPtr = std::make_shared<ZDD<SylinderNearEP>>(sylinderContainer.getNumberOfParticleLocal());

    treeSylinderNumber = 0;
    setTreeSylinder();

    setPosWithWall();

    calcVolFrac();

    if (commRcp->getRank() == 0) {
        IOHelper::makeSubFolder("./result"); // prepare the output directory
        writeBox();
    }

    if (!runConfig.sylinderFixed) {
        // 100 NON-B steps to resolve initial configuration collisions
        // no output
        if (commRcp->getRank() == 0) {
            printf("-------------------------------------\n");
            printf("-Initial Collision Resolution Begin--\n");
            printf("-------------------------------------\n");
        }
        for (int i = 0; i < 100; i++) {
            prepareStep();
            calcVelocityNonCon();
            resolveConstraints();
            saveVelocityConstraints();
            sumVelocity();
            stepEuler();
        }
        if (commRcp->getRank() == 0) {
            printf("--Initial Collision Resolution End---\n");
            printf("-------------------------------------\n");
        }
    }

    printf("SylinderSystem Initialized. %d sylinders on process %d\n", sylinderContainer.getNumberOfParticleLocal(),
           commRcp->getRank());
}

void SylinderSystem::setTreeSylinder() {
    // initialize tree
    // always keep tree max_glb_num_ptcl to be twice the global actual particle number.
    const int nGlobal = sylinderContainer.getNumberOfParticleGlobal();
    if (nGlobal > 1.5 * treeSylinderNumber || !treeSylinderNearPtr) {
        // a new larger tree
        treeSylinderNearPtr.reset();
        treeSylinderNearPtr = std::make_unique<TreeSylinderNear>();
        treeSylinderNearPtr->initialize(2 * nGlobal);
        treeSylinderNumber = nGlobal;
    }
}

void SylinderSystem::getOrient(Equatn &orient, const double px, const double py, const double pz, const int threadId) {
    Evec3 pvec;
    if (px < -1 || px > 1) {
        pvec[0] = 2 * rngPoolPtr->getU01(threadId) - 1;
    } else {
        pvec[0] = px;
    }
    if (py < -1 || py > 1) {
        pvec[1] = 2 * rngPoolPtr->getU01(threadId) - 1;
    } else {
        pvec[1] = py;
    }
    if (pz < -1 || pz > 1) {
        pvec[2] = 2 * rngPoolPtr->getU01(threadId) - 1;
    } else {
        pvec[2] = pz;
    }

    // px,py,pz all random, pick uniformly in orientation space
    if (px != pvec[0] && py != pvec[1] && pz != pvec[2]) {
        EquatnHelper::setUnitRandomEquatn(orient, rngPoolPtr->getU01(threadId), rngPoolPtr->getU01(threadId),
                                          rngPoolPtr->getU01(threadId));
        return;
    } else {
        orient = Equatn::FromTwoVectors(Evec3(0, 0, 1), pvec);
    }
}

void SylinderSystem::setInitialFromConfig() {
    // this function init all sylinders on rank 0
    if (runConfig.sylinderLengthSigma > 0) {
        rngPoolPtr->setLogNormalParameters(runConfig.sylinderLength, runConfig.sylinderLengthSigma);
    }

    if (commRcp->getRank() != 0) {
        sylinderContainer.setNumberOfParticleLocal(0);
    } else {
        const double boxEdge[3] = {runConfig.initBoxHigh[0] - runConfig.initBoxLow[0],
                                   runConfig.initBoxHigh[1] - runConfig.initBoxLow[1],
                                   runConfig.initBoxHigh[2] - runConfig.initBoxLow[2]};
        const double minBoxEdge = std::min(std::min(boxEdge[0], boxEdge[1]), boxEdge[2]);
        const double maxLength = minBoxEdge * 0.5;
        const double radius = runConfig.sylinderDiameter / 2;
        const int nSylinderLocal = runConfig.sylinderNumber;
        sylinderContainer.setNumberOfParticleLocal(nSylinderLocal);

#pragma omp parallel
        {
            const int threadId = omp_get_thread_num();
#pragma omp for
            for (int i = 0; i < nSylinderLocal; i++) {
                double length;
                if (runConfig.sylinderLengthSigma > 0) {
                    do { // generate random length
                        length = rngPoolPtr->getLN(threadId);
                    } while (length >= maxLength);
                } else {
                    length = runConfig.sylinderLength;
                }
                double pos[3];
                for (int k = 0; k < 3; k++) {
                    pos[k] = rngPoolPtr->getU01(threadId) * boxEdge[k] + runConfig.initBoxLow[k];
                }
                Equatn orientq;
                getOrient(orientq, runConfig.initOrient[0], runConfig.initOrient[1], runConfig.initOrient[2], threadId);
                double orientation[4];
                Emapq(orientation).coeffs() = orientq.coeffs();
                sylinderContainer[i] = Sylinder(i, radius, radius, length, length, pos, orientation);
                sylinderContainer[i].clear();
            }
        }
    }

    if (runConfig.initCircularX) {
        setInitialCircularCrossSection();
    }
}

void SylinderSystem::setInitialCircularCrossSection() {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    double radiusCrossSec = 0;            // x, y, z, axis radius
    Evec3 centerCrossSec = Evec3::Zero(); // x, y, z, axis center.
    // x axis
    centerCrossSec = Evec3(0, (runConfig.initBoxHigh[1] - runConfig.initBoxLow[1]) * 0.5 + runConfig.initBoxLow[1],
                           (runConfig.initBoxHigh[2] - runConfig.initBoxLow[2]) * 0.5 + runConfig.initBoxLow[2]);
    radiusCrossSec = 0.5 * std::min(runConfig.initBoxHigh[2] - runConfig.initBoxLow[2],
                                    runConfig.initBoxHigh[1] - runConfig.initBoxLow[1]);
#pragma omp parallel
    {
        const int threadId = omp_get_thread_num();
#pragma omp for
        for (int i = 0; i < nLocal; i++) {
            double y = sylinderContainer[i].pos[1];
            double z = sylinderContainer[i].pos[2];
            // replace y,z with position in the circle
            getRandPointInCircle(radiusCrossSec, rngPoolPtr->getU01(threadId), rngPoolPtr->getU01(threadId), y, z);
            sylinderContainer[i].pos[1] = y + centerCrossSec[1];
            sylinderContainer[i].pos[2] = z + centerCrossSec[2];
        }
    }
}

void SylinderSystem::calcVolFrac() {
    // calc volume fraction of sphero cylinders
    // step 1, calc local total volume
    double volLocal = 0;
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
#pragma omp parallel for reduction(+ : volLocal)
    for (int i = 0; i < nLocal; i++) {
        auto &sy = sylinderContainer[i];
        volLocal += 3.1415926535 * (0.25 * sy.length * pow(sy.radius * 2, 2) + pow(sy.radius * 2, 3) / 6);
    }
    double volGlobal = 0;

    Teuchos::reduceAll(*commRcp, Teuchos::SumValueReductionOp<int, double>(), 1, &volLocal, &volGlobal);

    // step 2, reduce to root and compute total volume
    if (commRcp->getRank() == 0) {
        double boxVolume = (runConfig.simBoxHigh[0] - runConfig.simBoxLow[0]) *
                           (runConfig.simBoxHigh[1] - runConfig.simBoxLow[1]) *
                           (runConfig.simBoxHigh[2] - runConfig.simBoxLow[2]);
        std::cout << "Volume Sylinder = " << volGlobal << std::endl;
        std::cout << "Volume fraction = " << volGlobal / boxVolume << std::endl;
    }
}

void SylinderSystem::setInitialFromFile(const std::string &filename) {
    if (commRcp->getRank() != 0) {
        sylinderContainer.setNumberOfParticleLocal(0);
    } else {
        std::ifstream myfile(filename);
        std::string line;
        std::getline(myfile, line); // read two header lines
        std::getline(myfile, line);

        std::vector<Sylinder> sylinderReadFromFile;
        while (std::getline(myfile, line)) {
            char typeChar;
            std::istringstream liness(line);
            liness >> typeChar;
            if (typeChar == 'C') {
                Sylinder newBody;
                int gid;
                double mx, my, mz;
                double px, py, pz;
                double radius;
                liness >> gid >> radius >> mx >> my >> mz >> px >> py >> pz;
                Emap3(newBody.pos) = Evec3((mx + px) / 2, (my + py) / 2, (mz + pz) / 2);
                newBody.gid = gid;
                newBody.length = sqrt((px - mx) * (px - mx) + (py - my) * (py - my) + (pz - mz) * (pz - mz));
                Evec3 direction(px - mx, py - my, pz - mz);
                Emapq(newBody.orientation) = Equatn::FromTwoVectors(Evec3(0, 0, 1), direction);

                newBody.radius = radius;
                newBody.radiusCollision = radius;
                newBody.lengthCollision = newBody.length;
                sylinderReadFromFile.push_back(newBody);
                typeChar = 'N';
            }
        }
        myfile.close();

        // sort body, gid ascending;
        std::cout << "Sylinder number in file: " << sylinderReadFromFile.size() << std::endl;
        std::sort(sylinderReadFromFile.begin(), sylinderReadFromFile.end(),
                  [](const Sylinder &t1, const Sylinder &t2) { return t1.gid < t2.gid; });

        // set local
        const int nRead = sylinderReadFromFile.size();
        sylinderContainer.setNumberOfParticleLocal(nRead);
#pragma omp parallel for
        for (int i = 0; i < nRead; i++) {
            sylinderContainer[i] = sylinderReadFromFile[i];
            sylinderContainer[i].clear();
        }
    }
}

std::string SylinderSystem::getCurrentResultFolder() {
    const int num = std::max(400 / commRcp->getSize(), 1); // limit max number of files per folder
    int k = snapID / num;
    int low = k * num, high = k * num + num - 1;
    std::string baseFolder =
        "./result/result" + std::to_string(low) + std::string("-") + std::to_string(high) + std::string("/");
    return baseFolder;
}

void SylinderSystem::writeAscii(const std::string &baseFolder) {
    // write a single ascii .dat file
    const int nGlobal = sylinderContainer.getNumberOfParticleGlobal();

    std::string name = baseFolder + std::string("SylinderAscii_") + std::to_string(snapID) + ".dat";
    SylinderAsciiHeader header;
    header.nparticle = nGlobal;
    header.time = stepCount * runConfig.dt;
    sylinderContainer.writeParticleAscii(name.c_str(), header);
}

void SylinderSystem::writeVTK(const std::string &baseFolder) {
    const int rank = commRcp->getRank();
    const int size = commRcp->getSize();
    Sylinder::writeVTP<PS::ParticleSystem<Sylinder>>(sylinderContainer, sylinderContainer.getNumberOfParticleLocal(),
                                                     baseFolder, std::to_string(snapID), rank);
    uniConstraintPtr->writeVTP(baseFolder, "Col", std::to_string(snapID), rank);
    biConstraintPtr->writeVTP(baseFolder, "Bi", std::to_string(snapID), rank);
    if (rank == 0) {
        Sylinder::writePVTP(baseFolder, std::to_string(snapID), size); // write parallel head
        uniConstraintPtr->writePVTP(baseFolder, "Col", std::to_string(snapID), size);
        biConstraintPtr->writePVTP(baseFolder, "Bi", std::to_string(snapID), size);
    }
}

void SylinderSystem::writeBox() {
    FILE *boxFile = fopen("./result/simBox.vtk", "w");
    fprintf(boxFile, "# vtk DataFile Version 3.0\n");
    fprintf(boxFile, "vtk file\n");
    fprintf(boxFile, "ASCII\n");
    fprintf(boxFile, "DATASET RECTILINEAR_GRID\n");
    fprintf(boxFile, "DIMENSIONS 2 2 2\n");
    fprintf(boxFile, "X_COORDINATES 2 float\n");
    fprintf(boxFile, "%g %g\n", runConfig.simBoxLow[0], runConfig.simBoxHigh[0]);
    fprintf(boxFile, "Y_COORDINATES 2 float\n");
    fprintf(boxFile, "%g %g\n", runConfig.simBoxLow[1], runConfig.simBoxHigh[1]);
    fprintf(boxFile, "Z_COORDINATES 2 float\n");
    fprintf(boxFile, "%g %g\n", runConfig.simBoxLow[2], runConfig.simBoxHigh[2]);
    fprintf(boxFile, "CELL_DATA 1\n");
    fprintf(boxFile, "POINT_DATA 8\n");
    fclose(boxFile);
}

void SylinderSystem::writeResult() {
    std::string baseFolder = getCurrentResultFolder();
    IOHelper::makeSubFolder(baseFolder);
    writeAscii(baseFolder);
    writeVTK(baseFolder);
    snapID++;
}

void SylinderSystem::showOnScreenRank0() {
    if (commRcp->getRank() == 0) {
        printf("-----------SylinderSystem Settings-----------\n");
        runConfig.dump();
        printf("-----------Sylinder Configurations-----------\n");
        const int nLocal = sylinderContainer.getNumberOfParticleLocal();
        for (int i = 0; i < nLocal; i++) {
            sylinderContainer[i].dumpSylinder();
        }
    }
    commRcp->barrier();
}

void SylinderSystem::setDomainInfo() {
    const int pbcX = (runConfig.simBoxPBC[0] ? 1 : 0);
    const int pbcY = (runConfig.simBoxPBC[1] ? 1 : 0);
    const int pbcZ = (runConfig.simBoxPBC[2] ? 1 : 0);
    const int pbcFlag = 100 * pbcX + 10 * pbcY + pbcZ;

    switch (pbcFlag) {
    case 0:
        dinfo.setBoundaryCondition(PS::BOUNDARY_CONDITION_OPEN);
        break;
    case 1:
        dinfo.setBoundaryCondition(PS::BOUNDARY_CONDITION_PERIODIC_Z);
        break;
    case 10:
        dinfo.setBoundaryCondition(PS::BOUNDARY_CONDITION_PERIODIC_Y);
        break;
    case 100:
        dinfo.setBoundaryCondition(PS::BOUNDARY_CONDITION_PERIODIC_X);
        break;
    case 11:
        dinfo.setBoundaryCondition(PS::BOUNDARY_CONDITION_PERIODIC_YZ);
        break;
    case 101:
        dinfo.setBoundaryCondition(PS::BOUNDARY_CONDITION_PERIODIC_XZ);
        break;
    case 110:
        dinfo.setBoundaryCondition(PS::BOUNDARY_CONDITION_PERIODIC_XY);
        break;
    case 111:
        dinfo.setBoundaryCondition(PS::BOUNDARY_CONDITION_PERIODIC_XYZ);
        break;
    }

    PS::F64vec3 rootDomainLow;
    PS::F64vec3 rootDomainHigh;
    for (int k = 0; k < 3; k++) {
        rootDomainLow[k] = runConfig.simBoxLow[k];
        rootDomainHigh[k] = runConfig.simBoxHigh[k];
    }

    dinfo.setPosRootDomain(rootDomainLow, rootDomainHigh); // rootdomain must be specified after PBC
}

void SylinderSystem::decomposeDomain() {
    applyBoxBC();
    dinfo.decomposeDomainAll(sylinderContainer);
}

void SylinderSystem::exchangeSylinder() {
    sylinderContainer.exchangeParticle(dinfo);
    updateSylinderRank();
}

void SylinderSystem::calcMobMatrix() {
    // diagonal hydro mobility operator
    // 3*3 block for translational + 3*3 block for rotational.
    // 3 nnz per row, 18 nnz per tubule

    const double Pi = 3.14159265358979323846;
    const double mu = runConfig.viscosity;

    const int nLocal = sylinderMapRcp->getNodeNumElements();
    TEUCHOS_ASSERT(nLocal == sylinderContainer.getNumberOfParticleLocal());
    const int localSize = nLocal * 6; // local row number

    Kokkos::View<size_t *> rowPointers("rowPointers", localSize + 1);
    rowPointers[0] = 0;
    for (int i = 1; i <= localSize; i++) {
        rowPointers[i] = rowPointers[i - 1] + 3;
    }
    Kokkos::View<int *> columnIndices("columnIndices", rowPointers[localSize]);
    Kokkos::View<double *> values("values", rowPointers[localSize]);

#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        const auto &sy = sylinderContainer[i];

        // calculate the Mob Trans and MobRot
        Emat3 MobTrans; //            double MobTrans[3][3];
        Emat3 MobRot;   //            double MobRot[3][3];
        Emat3 qq;
        Emat3 Imqq;
        Evec3 q = ECmapq(sy.orientation) * Evec3(0, 0, 1);
        qq = q * q.transpose();
        Imqq = Emat3::Identity() - qq;
        // std::cout << cell.orientation.w() << std::endl;
        // std::cout << qq << std::endl;
        // std::cout << Imqq << std::endl;

        const double length = sy.length;
        const double diameter = sy.radius * 2;
        const double b = -(1 + 2 * log(diameter * 0.5 / (length)));
        const double dragPara = 8 * Pi * length * mu / (2 * b);
        const double dragPerp = 8 * Pi * length * mu / (b + 2);
        const double dragRot = 2 * Pi * mu * length * length * length / (3 * (b + 2));
        const double dragParaInv = 1 / dragPara;
        const double dragPerpInv = 1 / dragPerp;
        const double dragRotInv = 1 / dragRot;

        MobTrans = dragParaInv * qq + dragPerpInv * Imqq;
        MobRot = dragRotInv * qq + dragRotInv * Imqq;
        // MobRot regularized to remove null space.
        // here it becomes identity matrix,
        // no effect on geometric constraints
        // no problem for axissymetric slender body.
        // this simplifies the rotational Brownian calculations.

        // column index is local index
        columnIndices[18 * i] = 6 * i; // line 1 of Mob Trans
        columnIndices[18 * i + 1] = 6 * i + 1;
        columnIndices[18 * i + 2] = 6 * i + 2;
        columnIndices[18 * i + 3] = 6 * i; // line 2 of Mob Trans
        columnIndices[18 * i + 4] = 6 * i + 1;
        columnIndices[18 * i + 5] = 6 * i + 2;
        columnIndices[18 * i + 6] = 6 * i; // line 3 of Mob Trans
        columnIndices[18 * i + 7] = 6 * i + 1;
        columnIndices[18 * i + 8] = 6 * i + 2;
        columnIndices[18 * i + 9] = 6 * i + 3; // line 1 of Mob Rot
        columnIndices[18 * i + 10] = 6 * i + 4;
        columnIndices[18 * i + 11] = 6 * i + 5;
        columnIndices[18 * i + 12] = 6 * i + 3; // line 2 of Mob Rot
        columnIndices[18 * i + 13] = 6 * i + 4;
        columnIndices[18 * i + 14] = 6 * i + 5;
        columnIndices[18 * i + 15] = 6 * i + 3; // line 3 of Mob Rot
        columnIndices[18 * i + 16] = 6 * i + 4;
        columnIndices[18 * i + 17] = 6 * i + 5;

        values[18 * i] = MobTrans(0, 0); // line 1 of Mob Trans
        values[18 * i + 1] = MobTrans(0, 1);
        values[18 * i + 2] = MobTrans(0, 2);
        values[18 * i + 3] = MobTrans(1, 0); // line 2 of Mob Trans
        values[18 * i + 4] = MobTrans(1, 1);
        values[18 * i + 5] = MobTrans(1, 2);
        values[18 * i + 6] = MobTrans(2, 0); // line 3 of Mob Trans
        values[18 * i + 7] = MobTrans(2, 1);
        values[18 * i + 8] = MobTrans(2, 2);
        values[18 * i + 9] = MobRot(0, 0); // line 1 of Mob Rot
        values[18 * i + 10] = MobRot(0, 1);
        values[18 * i + 11] = MobRot(0, 2);
        values[18 * i + 12] = MobRot(1, 0); // line 2 of Mob Rot
        values[18 * i + 13] = MobRot(1, 1);
        values[18 * i + 14] = MobRot(1, 2);
        values[18 * i + 15] = MobRot(2, 0); // line 3 of Mob Rot
        values[18 * i + 16] = MobRot(2, 1);
        values[18 * i + 17] = MobRot(2, 2);
    }

    // mobMat is block-diagonal, so domainMap=rangeMap
    mobilityMatrixRcp =
        Teuchos::rcp(new TCMAT(sylinderMobilityMapRcp, sylinderMobilityMapRcp, rowPointers, columnIndices, values));
    mobilityMatrixRcp->fillComplete(sylinderMobilityMapRcp, sylinderMobilityMapRcp); // domainMap, rangeMap

#ifdef DEBUGLCPCOL
    std::cout << "MobMat Constructed: " << mobilityMatrixRcp->description() << std::endl;
    dumpTCMAT(mobilityMatrixRcp, "MobMat.mtx");
#endif
}

void SylinderSystem::calcMobOperator() {
    calcMobMatrix();
    mobilityOperatorRcp = mobilityMatrixRcp;
}

void SylinderSystem::calcVelocityNonCon() {
    // velocityNonCon = velocityBrown + velocityPartNonBrown + mobility * forcePartNonBrown
    velocityNonConRcp = Teuchos::rcp<TV>(new TV(sylinderMobilityMapRcp, true)); // allocate and zero out

    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    TEUCHOS_ASSERT(nLocal * 6 == velocityNonConRcp->getLocalLength());

    if (!forcePartNonBrownRcp.is_null()) {
        TEUCHOS_ASSERT(!mobilityOperatorRcp.is_null());
        mobilityOperatorRcp->apply(*forcePartNonBrownRcp, *velocityNonConRcp);
    }

    if (!velocityNonBrownRcp.is_null()) {
        velocityNonConRcp->update(1.0, *velocityNonBrownRcp, 1.0);
    }

    // write back total non Brownian velocity
    // combine and sync the velNonB set in two places
    auto velPtr = velocityNonConRcp->getLocalView<Kokkos::HostSpace>();

#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        auto &sy = sylinderContainer[i];
        // translational
        sy.velNonB[0] = velPtr(6 * i + 0, 0);
        sy.velNonB[1] = velPtr(6 * i + 1, 0);
        sy.velNonB[2] = velPtr(6 * i + 2, 0);
        // rotational
        sy.omegaNonB[0] = velPtr(6 * i + 3, 0);
        sy.omegaNonB[1] = velPtr(6 * i + 4, 0);
        sy.omegaNonB[2] = velPtr(6 * i + 5, 0);
    }

    if (!velocityBrownRcp.is_null()) {
        velocityNonConRcp->update(1.0, *velocityBrownRcp, 1.0);
    }
}

void SylinderSystem::sumVelocity() {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        auto &sy = sylinderContainer[i];
        for (int k = 0; k < 3; k++) {
            sy.vel[k] = sy.velNonB[k] + sy.velBrown[k] + sy.velCol[k] + sy.velBi[k];
            sy.omega[k] = sy.omegaNonB[k] + sy.omegaBrown[k] + sy.omegaCol[k] + sy.omegaBi[k];
        }
    }
}

void SylinderSystem::stepEuler() {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    const double dt = runConfig.dt;

    if (!runConfig.sylinderFixed) {
#pragma omp parallel for
        for (int i = 0; i < nLocal; i++) {
            auto &sy = sylinderContainer[i];
            sy.stepEuler(dt);
        }
    }
}

void SylinderSystem::resolveConstraints() {

    Teuchos::RCP<Teuchos::Time> collectColTimer =
        Teuchos::TimeMonitor::getNewCounter("SylinderSystem::CollectCollision");
    Teuchos::RCP<Teuchos::Time> collectBiTimer =
        Teuchos::TimeMonitor::getNewCounter("SylinderSystem::CollectBilateral");
    if (enableTimer) {
        collectColTimer->enable();
        collectBiTimer->enable();
    } else {
        collectColTimer->disable();
        collectBiTimer->disable();
    }

    printRank0("start collect collisions");
    {
        Teuchos::TimeMonitor mon(*collectColTimer);
        collectPairCollision();
        collectWallCollision();
    }

    // printRank0("start collect bilaterals");
    // {
    //     Teuchos::TimeMonitor mon(*collectColTimer);
    //     collectLinkBilateral();
    // }

    // solve collision
    // positive buffer value means collision radius is effectively smaller
    // i.e., less likely to collide
    Teuchos::RCP<Teuchos::Time> solveTimer = Teuchos::TimeMonitor::getNewCounter("SylinderSystem::SolveConstraints");
    if (enableTimer) {
        solveTimer->enable();
    } else {
        solveTimer->disable();
    }
    {
        Teuchos::TimeMonitor mon(*solveTimer);
        const double buffer = 0;
        printRank0("constraint solver setup");
        constraintSolverPtr->setup(*uniConstraintPtr, *biConstraintPtr, mobilityOperatorRcp, velocityNonConRcp,
                                   runConfig.dt);
        printRank0("set control");
        constraintSolverPtr->setControlParams(runConfig.conResTol, runConfig.conMaxIte, runConfig.conSolverChoice);
        printRank0("solve");
        constraintSolverPtr->solveConstraints();
        printRank0("writeback");
        constraintSolverPtr->writebackGamma();
    }

    saveVelocityConstraints();
}

void SylinderSystem::updateSylinderMap() {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    // setup the new sylinderMap
    sylinderMapRcp = getTMAPFromLocalSize(nLocal, commRcp);
    sylinderMobilityMapRcp = getTMAPFromLocalSize(nLocal * 6, commRcp);

    // setup the globalIndex
    int globalIndexBase = sylinderMapRcp->getMinGlobalIndex(); // this is a contiguous map
#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        sylinderContainer[i].globalIndex = i + globalIndexBase;
    }
}

bool SylinderSystem::getIfWriteResultCurrentStep() {
    return (stepCount % static_cast<int>(runConfig.timeSnap / runConfig.dt) == 0);
}

void SylinderSystem::prepareStep() {
    applyBoxBC();

    if (stepCount % 50 == 0) {
        decomposeDomain();
    }

    exchangeSylinder();

    updateSylinderMap();

    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        sylinderContainer[i].radiusCollision = sylinderContainer[i].radius * runConfig.sylinderDiameterColRatio;
        sylinderContainer[i].lengthCollision = sylinderContainer[i].length * runConfig.sylinderLengthColRatio;
        sylinderContainer[i].clear();
    }

    // buildSylinderNearDataDirectory();

    // sylinderGidIndex.clear();
    // for (int i = 0; i < nLocal; i++) {
    //     sylinderGidIndex.emplace(sylinderContainer[i].gid, i);
    // }

    calcMobOperator();

    uniConstraintPtr->clear();
    biConstraintPtr->clear();

    forcePartNonBrownRcp.reset();
    velocityPartNonBrownRcp.reset();
    velocityNonBrownRcp.reset();
    velocityBrownRcp.reset();
}

void SylinderSystem::setForceNonBrown(const std::vector<double> &forceNonBrown) {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    TEUCHOS_ASSERT(forceNonBrown.size() == 6 * nLocal);
    TEUCHOS_ASSERT(sylinderMobilityMapRcp->getNodeNumElements() == 6 * nLocal);
    forcePartNonBrownRcp = getTVFromVector(forceNonBrown, commRcp);
}

void SylinderSystem::setVelocityNonBrown(const std::vector<double> &velNonBrown) {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    TEUCHOS_ASSERT(velNonBrown.size() == 6 * nLocal);
    TEUCHOS_ASSERT(sylinderMobilityMapRcp->getNodeNumElements() == 6 * nLocal);
    velocityPartNonBrownRcp = getTVFromVector(velNonBrown, commRcp);
}

void SylinderSystem::runStep() {

    if (runConfig.KBT > 0) {
        calcVelocityBrown();
    }

    calcVelocityNonCon();

    resolveConstraints();

    sumVelocity();

    if (getIfWriteResultCurrentStep()) {
        // write result before moving. guarantee data written is consistent to geometry
        writeResult();
    }

    stepEuler();

    stepCount++;
}

void SylinderSystem::saveVelocityConstraints() {
    // save results
    forceUniRcp = constraintSolverPtr->getForceUni();
    velocityUniRcp = constraintSolverPtr->getVelocityUni();
    forceBiRcp = constraintSolverPtr->getForceBi();
    velocityBiRcp = constraintSolverPtr->getVelocityBi();

    auto velUniPtr = velocityUniRcp->getLocalView<Kokkos::HostSpace>();
    auto velBiPtr = velocityBiRcp->getLocalView<Kokkos::HostSpace>();

    const int sylinderLocalNumber = sylinderContainer.getNumberOfParticleLocal();
    TEUCHOS_ASSERT(velUniPtr.dimension_0() == sylinderLocalNumber * 6);
    TEUCHOS_ASSERT(velUniPtr.dimension_1() == 1);
    TEUCHOS_ASSERT(velBiPtr.dimension_0() == sylinderLocalNumber * 6);
    TEUCHOS_ASSERT(velBiPtr.dimension_1() == 1);

#pragma omp parallel for
    for (int i = 0; i < sylinderLocalNumber; i++) {
        auto &sy = sylinderContainer[i];
        sy.velCol[0] = velUniPtr(6 * i, 0);
        sy.velCol[1] = velUniPtr(6 * i + 1, 0);
        sy.velCol[2] = velUniPtr(6 * i + 2, 0);
        sy.omegaCol[0] = velUniPtr(6 * i + 3, 0);
        sy.omegaCol[1] = velUniPtr(6 * i + 4, 0);
        sy.omegaCol[2] = velUniPtr(6 * i + 5, 0);
        sy.velBi[0] = velBiPtr(6 * i, 0);
        sy.velBi[1] = velBiPtr(6 * i + 1, 0);
        sy.velBi[2] = velBiPtr(6 * i + 2, 0);
        sy.omegaBi[0] = velBiPtr(6 * i + 3, 0);
        sy.omegaBi[1] = velBiPtr(6 * i + 4, 0);
        sy.omegaBi[2] = velBiPtr(6 * i + 5, 0);
    }
}

void SylinderSystem::calcVelocityBrown() {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    const double Pi = 3.1415926535897932384626433;
    const double mu = runConfig.viscosity;
    const double dt = runConfig.dt;
    const double delta = dt * 0.1; // a small parameter used in RFD algorithm
    const double kBT = runConfig.KBT;
    const double kBTfactor = sqrt(2 * kBT / dt);

#pragma omp parallel
    {
        const int threadId = omp_get_thread_num();
#pragma omp for
        for (int i = 0; i < nLocal; i++) {
            auto &sy = sylinderContainer[i];
            // constants
            const double length = sy.length;
            const double diameter = sy.radius * 2;
            const double b = -(1 + 2 * log(diameter * 0.5 / (length)));
            const double invDragPara = 1 / (8 * Pi * length * mu / (2 * b));
            const double invDragPerp = 1 / (8 * Pi * length * mu / (b + 2));
            const double invDragRot = 1 / (2 * Pi * mu * length * length * length / (3 * (b + 2)));

            // convert FDPS vec3 to Evec3
            Evec3 direction = Emapq(sy.orientation) * Evec3(0, 0, 1);

            // RFD from Delong, JCP, 2015
            // slender fiber has 0 rot drag, regularize with identity rot mobility
            // trans mobility is this
            Evec3 q = direction;
            Emat3 Nmat = (invDragPara - invDragPerp) * (q * q.transpose()) + (invDragPerp)*Emat3::Identity();
            Emat3 Nmatsqrt = Nmat.llt().matrixL();

            // velocity
            Evec3 Wrot(rngPoolPtr->getN01(threadId), rngPoolPtr->getN01(threadId), rngPoolPtr->getN01(threadId));
            Evec3 Wpos(rngPoolPtr->getN01(threadId), rngPoolPtr->getN01(threadId), rngPoolPtr->getN01(threadId));
            Evec3 Wrfdrot(rngPoolPtr->getN01(threadId), rngPoolPtr->getN01(threadId), rngPoolPtr->getN01(threadId));
            Evec3 Wrfdpos(rngPoolPtr->getN01(threadId), rngPoolPtr->getN01(threadId), rngPoolPtr->getN01(threadId));

            Equatn orientRFD = Emapq(sy.orientation);
            EquatnHelper::rotateEquatn(orientRFD, Wrfdrot, delta);
            q = orientRFD * Evec3(0, 0, 1);
            Emat3 Nmatrfd = (invDragPara - invDragPerp) * (q * q.transpose()) + (invDragPerp)*Emat3::Identity();

            Evec3 vel = kBTfactor * (Nmatsqrt * Wpos);           // Gaussian noise
            vel += (kBT / delta) * ((Nmatrfd - Nmat) * Wrfdpos); // rfd drift. seems no effect in this case
            Evec3 omega = sqrt(invDragRot) * kBTfactor * Wrot;   // regularized identity rotation drag

            Emap3(sy.velBrown) = vel;
            Emap3(sy.omegaBrown) = omega;
        }
    }

    velocityBrownRcp = Teuchos::rcp<TV>(new TV(sylinderMobilityMapRcp, true));
    auto velocityPtr = velocityBrownRcp->getLocalView<Kokkos::HostSpace>();
    velocityBrownRcp->modify<Kokkos::HostSpace>();

    TEUCHOS_ASSERT(velocityPtr.dimension_0() == nLocal * 6);
    TEUCHOS_ASSERT(velocityPtr.dimension_1() == 1);

#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        const auto &sy = sylinderContainer[i];
        velocityPtr(6 * i, 0) = sy.velBrown[0];
        velocityPtr(6 * i + 1, 0) = sy.velBrown[1];
        velocityPtr(6 * i + 2, 0) = sy.velBrown[2];
        velocityPtr(6 * i + 3, 0) = sy.omegaBrown[0];
        velocityPtr(6 * i + 4, 0) = sy.omegaBrown[1];
        velocityPtr(6 * i + 5, 0) = sy.omegaBrown[2];
    }
}

void SylinderSystem::collectWallCollision() {
    auto collisionPoolPtr = uniConstraintPtr->constraintPoolPtr; // shared_ptr
    const int nThreads = collisionPoolPtr->size();
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();

    if (runConfig.wallLowZ) {
        // process collisions with bottom wall
        const double wallBot = runConfig.simBoxLow[2];
#pragma omp parallel num_threads(nThreads)
        {
            const int threadId = omp_get_thread_num();
#pragma omp for
            for (int i = 0; i < nLocal; i++) {
                const auto &sy = sylinderContainer[i];
                const Evec3 direction = ECmapq(sy.orientation) * Evec3(0, 0, 1);
                const Evec3 Pm = ECmap3(sy.pos) - direction * (sy.lengthCollision * 0.5);
                const Evec3 Pp = ECmap3(sy.pos) + direction * (sy.lengthCollision * 0.5);
                const double distm = Pm[2] - wallBot - sy.radius;
                const double distp = Pp[2] - wallBot - sy.radius;
                // if collision, norm is always (0,0,1), loc could be Pm, Pp, or middle
                Evec3 colLoc;
                double phi0;
                bool wallCollide = false;
                if (distm < distp && distm < 0) {
                    colLoc = Pm;
                    phi0 = distm;
                    wallCollide = true;
                } else if (distm > distp && distp < 0) {
                    colLoc = Pp;
                    phi0 = distp;
                    wallCollide = true;
                } else if (distm == distp && distm < 0) {
                    colLoc = (Pm + Pp) * 0.5; // middle point
                    phi0 = distm;
                    wallCollide = true;
                }
                if (wallCollide != true) {
                    continue;
                }
                // add a new collision block. this block has only 6 non zero entries.
                // passing sy.gid+1/globalIndex+1 as a 'fake' colliding body j, which is actually not used in the solver
                // when oneside=true, out of range index is ignored
                (*collisionPoolPtr)[threadId].emplace_back(
                    phi0, -phi0, sy.gid, sy.gid + 1, sy.globalIndex, sy.globalIndex + 1, Evec3(0, 0, 1), Evec3(0, 0, 0),
                    colLoc - ECmap3(sy.pos), Evec3(0, 0, 0), colLoc, Evec3(colLoc[0], colLoc[1], wallBot), true);
            }
        }
    }

    if (runConfig.wallHighZ) {
        const double wallTop = runConfig.simBoxHigh[2];
        // process collisions with top wall
#pragma omp parallel num_threads(nThreads)
        {
            const int threadId = omp_get_thread_num();
#pragma omp for
            for (int i = 0; i < nLocal; i++) {
                const auto &sy = sylinderContainer[i];
                const Evec3 direction = ECmapq(sy.orientation) * Evec3(0, 0, 1);
                const Evec3 Pm = ECmap3(sy.pos) - direction * (sy.lengthCollision * 0.5);
                const Evec3 Pp = ECmap3(sy.pos) + direction * (sy.lengthCollision * 0.5);
                const double distm = wallTop - Pm[2] - sy.radius;
                const double distp = wallTop - Pp[2] - sy.radius;
                // if collision, norm is always (0,0,-1), loc could be Pm, Pp, or middle
                Evec3 colLoc;
                double phi0;
                bool wallCollide = false;
                if (distm < distp && distm < 0) {
                    colLoc = Pm;
                    phi0 = distm;
                    wallCollide = true;
                } else if (distm > distp && distp < 0) {
                    colLoc = Pp;
                    phi0 = distp;
                    wallCollide = true;
                } else if (distm == distp && distm < 0) {
                    colLoc = (Pm + Pp) * 0.5; // middle point
                    phi0 = distm;
                    wallCollide = true;
                }
                if (wallCollide != true) {
                    continue;
                }
                // add a new collision block. this block has only 6 non zero entries.
                // passing sy.gid+1/globalIndex+1 as a 'fake' colliding body j, which is actually not used in the solver
                // when oneside=true, out of range index is ignored
                (*collisionPoolPtr)[threadId].emplace_back(phi0, -phi0, sy.gid, sy.gid + 1, sy.globalIndex,
                                                           sy.globalIndex + 1, Evec3(0, 0, -1), Evec3(0, 0, 0),
                                                           colLoc - ECmap3(sy.pos), Evec3(0, 0, 0), colLoc,
                                                           Evec3(colLoc[0], colLoc[1], wallTop), true);
            }
        }
    }

    return;
}

void SylinderSystem::collectPairCollision() {

    CalcSylinderNearForce calcColFtr(uniConstraintPtr->constraintPoolPtr, biConstraintPtr->constraintPoolPtr);

    TEUCHOS_ASSERT(treeSylinderNearPtr);
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    setTreeSylinder();
    treeSylinderNearPtr->calcForceAll(calcColFtr, sylinderContainer, dinfo);

#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        sylinderContainer[i].sepmin = (treeSylinderNearPtr->getForce(i)).sepmin;
    }

    const int nQue = biConstraintPtr->constraintPoolPtr->size();
#pragma omp parallel for
    for (int q = 0; q < nQue; q++) {
        auto &queue = biConstraintPtr->constraintPoolPtr->at(q);
        for (auto &block : queue) {
            if (block.kappa < 0) {
                block.kappa = runConfig.linkKappa;
                block.gamma = block.kappa * block.delta0;
            }
        }
    }
}

std::pair<int, int> SylinderSystem::getMaxGid() {
    int maxGidLocal = 0;
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    for (int i = 0; i < nLocal; i++) {
        maxGidLocal = std::max(maxGidLocal, sylinderContainer[i].gid);
    }

    int maxGidGlobal = maxGidLocal;
    Teuchos::reduceAll(*commRcp, Teuchos::MaxValueReductionOp<int, int>(), 1, &maxGidLocal, &maxGidGlobal);
    if (commRcp->getRank() == 0)
        printf("rank: %d,maxGidLocal: %d,maxGidGlobal %d\n", commRcp->getRank(), maxGidLocal, maxGidGlobal);

    return std::pair<int, int>(maxGidLocal, maxGidGlobal);
}

void SylinderSystem::calcBoundingBox(double localLow[3], double localHigh[3], double globalLow[3],
                                     double globalHigh[3]) {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    double lx, ly, lz;
    lx = ly = lz = std::numeric_limits<double>::max();
    double hx, hy, hz;
    hx = hy = hz = std::numeric_limits<double>::min();

    for (int i = 0; i < nLocal; i++) {
        const auto &sy = sylinderContainer[i];
        const Evec3 direction = ECmapq(sy.orientation) * Evec3(0, 0, 1);
        Evec3 pm = ECmap3(sy.pos) - (sy.length * 0.5) * direction;
        Evec3 pp = ECmap3(sy.pos) + (sy.length * 0.5) * direction;
        lx = std::min(std::min(lx, pm[0]), pp[0]);
        ly = std::min(std::min(ly, pm[1]), pp[1]);
        lz = std::min(std::min(lz, pm[2]), pp[2]);
        hx = std::max(std::max(hx, pm[0]), pp[0]);
        hy = std::max(std::max(hy, pm[1]), pp[1]);
        hz = std::max(std::max(hz, pm[2]), pp[2]);
    }

    localLow[0] = lx;
    localLow[1] = ly;
    localLow[2] = lz;
    localHigh[0] = hx;
    localHigh[1] = hy;
    localHigh[2] = hz;

    for (int k = 0; k < 3; k++) {
        globalLow[k] = localLow[k];
        globalHigh[k] = localHigh[k];
    }

    Teuchos::reduceAll(*commRcp, Teuchos::MinValueReductionOp<int, double>(), 3, localLow, globalLow);
    Teuchos::reduceAll(*commRcp, Teuchos::MaxValueReductionOp<int, double>(), 3, localHigh, globalHigh);

    return;
}

void SylinderSystem::updateSylinderRank() {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    const int rank = commRcp->getRank();
#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        sylinderContainer[i].rank = rank;
    }
}

void SylinderSystem::applyBoxBC() { sylinderContainer.adjustPositionIntoRootDomain(dinfo); }

void SylinderSystem::calcColStress() {

    Emat3 meanStress = Emat3::Zero();
    uniConstraintPtr->sumLocalConstraintStress(meanStress, false);

    // scale to nkBT
    const double scaleFactor = 1 / (sylinderMapRcp->getGlobalNumElements() * runConfig.KBT);
    meanStress *= scaleFactor;
    // mpi reduction
    double meanStressLocal[9];
    double meanStressGlobal[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            meanStressLocal[i * 3 + j] = meanStress(i, j);
            meanStressGlobal[i * 3 + j] = 0;
        }
    }

    Teuchos::reduceAll(*commRcp, Teuchos::SumValueReductionOp<int, double>(), 9, meanStressLocal, meanStressGlobal);

    if (commRcp->getRank() == 0)
        printf("RECORD: ColXF,%7g,%7g,%7g,%7g,%7g,%7g,%7g,%7g,%7g\n",         //
               meanStressGlobal[0], meanStressGlobal[1], meanStressGlobal[2], //
               meanStressGlobal[3], meanStressGlobal[4], meanStressGlobal[5], //
               meanStressGlobal[6], meanStressGlobal[7], meanStressGlobal[8]);
}

void SylinderSystem::calcBiStress() {

    Emat3 meanStress = Emat3::Zero();
    biConstraintPtr->sumLocalConstraintStress(meanStress, false);

    // scale to nkBT
    const double scaleFactor = 1 / (sylinderMapRcp->getGlobalNumElements() * runConfig.KBT);
    meanStress *= scaleFactor;
    // mpi reduction
    double meanStressLocal[9];
    double meanStressGlobal[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            meanStressLocal[i * 3 + j] = meanStress(i, j);
            meanStressGlobal[i * 3 + j] = 0;
        }
    }

    Teuchos::reduceAll(*commRcp, Teuchos::SumValueReductionOp<int, double>(), 9, meanStressLocal, meanStressGlobal);

    if (commRcp->getRank() == 0)
        printf("RECORD: BiXF,%7g,%7g,%7g,%7g,%7g,%7g,%7g,%7g,%7g\n",          //
               meanStressGlobal[0], meanStressGlobal[1], meanStressGlobal[2], //
               meanStressGlobal[3], meanStressGlobal[4], meanStressGlobal[5], //
               meanStressGlobal[6], meanStressGlobal[7], meanStressGlobal[8]);
}

void SylinderSystem::calcOrderParameter() {
    double px = 0, py = 0, pz = 0;    // pvec
    double Qxx = 0, Qxy = 0, Qxz = 0; // Qtensor
    double Qyx = 0, Qyy = 0, Qyz = 0; // Qtensor
    double Qzx = 0, Qzy = 0, Qzz = 0; // Qtensor

    const int nLocal = sylinderContainer.getNumberOfParticleLocal();

#pragma omp parallel for reduction(+ : px, py, pz, Qxx, Qxy, Qxz, Qyx, Qyy, Qyz, Qzx, Qzy, Qzz)
    for (int i = 0; i < nLocal; i++) {
        const auto &sy = sylinderContainer[i];
        const Evec3 direction = ECmapq(sy.orientation) * Evec3(0, 0, 1);
        px += direction.x();
        py += direction.y();
        pz += direction.z();
        const Emat3 Q = direction * direction.transpose() - Emat3::Identity() * (1 / 3.0);
        Qxx += Q(0, 0);
        Qxy += Q(0, 1);
        Qxz += Q(0, 2);
        Qyx += Q(1, 0);
        Qyy += Q(1, 1);
        Qyz += Q(1, 2);
        Qzx += Q(2, 0);
        Qzy += Q(2, 1);
        Qzz += Q(2, 2);
    }

    // global average
    const int nGlobal = sylinderContainer.getNumberOfParticleGlobal();
    double pQ[12] = {px, py, pz, Qxx, Qxy, Qxz, Qyx, Qyy, Qyz, Qzx, Qzy, Qzz};
    MPI_Allreduce(MPI_IN_PLACE, pQ, 12, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    for (int i = 0; i < 12; i++) {
        pQ[i] *= (1.0 / nGlobal);
    }

    if (commRcp()->getRank() == 0) {
        printf("RECORD: Order P,%6g,%6g,%6g,Q,%6g,%6g,%6g,%6g,%6g,%6g,%6g,%6g,%6g\n", //
               pQ[0], pQ[1], pQ[2],                                                   // pvec
               pQ[3], pQ[4], pQ[5],                                                   // Qtensor
               pQ[6], pQ[7], pQ[8],                                                   // Qtensor
               pQ[9], pQ[10], pQ[11]                                                  // Qtensor
        );
    }
}

void SylinderSystem::setPosWithWall() {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    const double buffer = 1e-4;
    // directly move sylinders to avoid overlapping with the wall
    if (runConfig.wallLowZ) {
        const double wallBot = runConfig.simBoxLow[2];
#pragma omp parallel for
        for (int i = 0; i < nLocal; i++) {
            auto &sy = sylinderContainer[i];
            const Evec3 direction = Emapq(sy.orientation) * Evec3(0, 0, 1);
            const Evec3 Pm = Emap3(sy.pos) - direction * (sy.lengthCollision * 0.5);
            const Evec3 Pp = Emap3(sy.pos) + direction * (sy.lengthCollision * 0.5);
            const double distm = Pm[2] - sy.radius - wallBot;
            const double distp = Pp[2] - sy.radius - wallBot;
            if (distm < distp && distm < 0) {
                Emap3(sy.pos) += Evec3(0, 0, -distm + buffer);
            } else if (distp <= distm && distp < 0) {
                Emap3(sy.pos) += Evec3(0, 0, -distp + buffer);
            }
        }
    }

    if (runConfig.wallHighZ) {
        const double wallTop = runConfig.simBoxHigh[2];
#pragma omp parallel for
        for (int i = 0; i < nLocal; i++) {
            auto &sy = sylinderContainer[i];
            const Evec3 direction = Emapq(sy.orientation) * Evec3(0, 0, 1);
            const Evec3 Pm = Emap3(sy.pos) - direction * (sy.lengthCollision * 0.5);
            const Evec3 Pp = Emap3(sy.pos) + direction * (sy.lengthCollision * 0.5);
            const double distm = wallTop - (Pm[2] + sy.radius);
            const double distp = wallTop - (Pp[2] + sy.radius);
            if (distm < distp && distm < 0) {
                Emap3(sy.pos) -= Evec3(0, 0, -distm + buffer);
            } else if (distp <= distm && distp < 0) {
                Emap3(sy.pos) -= Evec3(0, 0, -distp + buffer);
            }
        }
    }
}

void SylinderSystem::addNewSylinder(std::vector<Sylinder> &newSylinder, std::vector<Link> &linkage) {
    // assign unique new gid for sylinders on all ranks
    std::pair<int, int> maxGid = getMaxGid();
    const int maxGidLocal = maxGid.first;
    const int maxGidGlobal = maxGid.second;
    const int newNumberOnLocal = newSylinder.size();
    const auto &newMapRcp = getTMAPFromLocalSize(newNumberOnLocal, commRcp);

    // a large enough buffer on every rank
    std::vector<int> newID(newMapRcp->getGlobalNumElements(), 0);
    std::vector<int> newNumber(commRcp->getSize(), 0);
    std::vector<int> displ(commRcp->getSize(), 0);

    // assign random id on rank 0
    if (commRcp->getRank() == 0) {
        std::iota(newID.begin(), newID.end(), 0);
        std::random_shuffle(newID.begin(), newID.end());
    }
    // collect number of ids from all ranks to rank0
    MPI_Gather(&newNumberOnLocal, 1, MPI_INT, newNumber.data(), 1, MPI_INT, 0, MPI_COMM_WORLD);

    if (commRcp->getRank() == 0) {
        std::partial_sum(newNumber.cbegin(), newNumber.cend() - 1, displ.begin() + 1);
    }

    std::vector<int> newIDRecv(newNumberOnLocal, 0);
    // scatter from rank 0 to every rank
    MPI_Scatterv(newID.data(), newNumber.data(), displ.data(), MPI_INT, //
                 newIDRecv.data(), newNumberOnLocal, MPI_INT, 0, MPI_COMM_WORLD);

    // set new gid
    for (int i = 0; i < newNumberOnLocal; i++) {
        newSylinder[i].gid = newIDRecv[i] + 1 + maxGidGlobal;
    }

    // set link connection
    if (linkage.size() == newNumberOnLocal) {
        for (int i = 0; i < newNumberOnLocal; i++) {
            newSylinder[i].link.group = linkage[i].group;
            newSylinder[i].link.prev =
                (linkage[i].prev == GEO_INVALID_INDEX) ? GEO_INVALID_INDEX : newSylinder[linkage[i].prev].gid;
            newSylinder[i].link.next =
                (linkage[i].next == GEO_INVALID_INDEX) ? GEO_INVALID_INDEX : newSylinder[linkage[i].next].gid;
        }
    } else if (linkage.size() == 0) {
        // no linkage, do nothing
    } else {
        printf("wrong linkage on rank: %d\n", commRcp->getRank());
        std::exit(1);
    }

    // add new cell to old cell
    for (int i = 0; i < newNumberOnLocal; i++) {
        sylinderContainer.addOneParticle(newSylinder[i]);
    }
}

void SylinderSystem::printRank0(const std::string &message) {
    if (commRcp->getRank() == 0) {
        std::cout << message << std::endl;
    }
}

void SylinderSystem::buildSylinderNearDataDirectory() {
    const size_t nLocal = sylinderContainer.getNumberOfParticleLocal();
    auto &sylinderNearDataDirectory = *sylinderNearDataDirectoryPtr;
    sylinderNearDataDirectory.gidOnLocal.resize(nLocal);
    sylinderNearDataDirectory.dataOnLocal.resize(nLocal);
#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
        sylinderNearDataDirectory.gidOnLocal[i] = sylinderContainer[i].gid;
        sylinderNearDataDirectory.dataOnLocal[i].copyFromFP(sylinderContainer[i]);
    }

    // build index
    sylinderNearDataDirectory.buildIndex();
}

void SylinderSystem::collectLinkBilateral() {
    // WARNING: periodic boundary condition is missing in this function. do not use.
    auto &cPool = *(biConstraintPtr->constraintPoolPtr);
    const int nThreads = cPool.size();
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    const double kappa = runConfig.linkKappa;
    // loop all links.
    // add constraint block for each link where next != INVALID

    // step 1, fill info where the next link is also on local mpi rank

#pragma omp parallel num_threads(nThreads)
    {
        const int tid = omp_get_thread_num();
        auto &cQue = cPool[tid];
        cQue.clear();
#pragma omp for
        for (int i = 0; i < nLocal; i++) {
            const auto &syI = sylinderContainer[i];
            if (syI.link.next == GEO_INVALID_INDEX) {
                continue; // no link, do nothing
            }

            const auto &J = sylinderGidIndex.find(syI.link.next);
            if (J != sylinderGidIndex.end()) { // syJ is also on local. add to cQue
                const Evec3 centerI = ECmap3(syI.pos);
                const Evec3 directionI = ECmapq(syI.orientation) * Evec3(0, 0, 1);
                // const Evec3 Pm = centerI - directionI * (0.5 * syI.length); // tail
                const Evec3 Pp = centerI + directionI * (0.5 * syI.length); // head
                const auto &syJ = sylinderContainer[J->second];
                const Evec3 centerJ = ECmap3(syJ.pos);
                const Evec3 directionJ = ECmapq(syJ.orientation) * Evec3(0, 0, 1);
                const Evec3 Qm = centerJ - directionJ * (0.5 * syJ.length); // tail
                // const Evec3 Qp = centerJ + directionJ * (0.5 * syJ.length); // head
                Evec3 Ploc = Pp; // head of I is linked to tail of J
                Evec3 Qloc = Qm;
                const Evec3 vecIJ = Ploc - Qloc;
                const double dist = vecIJ.norm();
                const Evec3 normI = vecIJ / dist;
                const Evec3 normJ = -normI;
                const Evec3 posI = Ploc - centerI;
                const Evec3 posJ = Qloc - centerJ;
                const double sep = dist - (syI.radius + syJ.radius) * 1.05; // L - L0
                double gamma = -sep * kappa;
                cQue.emplace_back(sep, gamma,       // L - L0, initial guess of gamma
                                  syI.gid, syJ.gid, //
                                  syI.globalIndex,  //
                                  syJ.globalIndex,  //
                                  normI, normJ,     // direction of collision force
                                  posI, posJ,       // location of collision relative to particle center
                                  Ploc, Qloc,       // location of collision in lab frame
                                  false, kappa);
                Emat3 stressIJ;
                CalcSylinderNearForce::collideStress(directionI, directionJ, centerI, centerJ, syI.length, syJ.length,
                                                     syI.radius, syJ.radius, 1.0, Ploc, Qloc, stressIJ);
                cQue.back().setStress(stressIJ);

            } else { // syJ is not on local. add syI info to block only
                printf("%d %d\n", syI.gid, syI.link.next);
                cQue.emplace_back(0, 0,                           // L - L0, initial guess of gamma
                                  syI.gid, syI.link.next,         //
                                  syI.globalIndex,                //
                                  GEO_INVALID_INDEX,              //
                                  Evec3(0, 0, 1), Evec3(0, 0, 1), // direction of collision force
                                  Evec3(0, 0, 1), Evec3(0, 0, 1), // location of collision relative to particle center
                                  Evec3(0, 0, 1), Evec3(0, 0, 1), // location of collision in lab frame
                                  false);
            }
        }
    }

    // step 2, fill missing information with DataDirectory from other mpi ranks.
    sylinderNearDataDirectoryPtr->gidToFind.clear();
    sylinderNearDataDirectoryPtr->dataToFind.clear();
    for (auto &cQue : cPool) {
        for (auto &block : cQue) {
            if (block.globalIndexJ == GEO_INVALID_INDEX) {
                sylinderNearDataDirectoryPtr->gidToFind.push_back(block.gidJ);
            }
        }
    }
    sylinderNearDataDirectoryPtr->find();
    int findIndex = 0;
    for (auto &cQue : cPool) {
        for (auto &block : cQue) {
            if (block.globalIndexJ == GEO_INVALID_INDEX) {
                auto I = sylinderGidIndex.find(block.gidI);
                if (I == sylinderGidIndex.end()) {
                    printf("sylinderGidIndex Error 1\n");
                    std::exit(1);
                }
                const auto &syI = sylinderContainer[I->second];
                const auto &syNearJ = sylinderNearDataDirectoryPtr->dataToFind[findIndex];
                if (syNearJ.gid != block.gidJ) {
                    printf("sylinderGidIndex Error 2\n");
                    std::exit(1);
                }

                const Evec3 centerI = ECmap3(syI.pos);
                const Evec3 directionI = ECmapq(syI.orientation) * Evec3(0, 0, 1);
                // const Evec3 Pm = centerI - directionI * (0.5 * syI.length); // tail
                const Evec3 Pp = centerI + directionI * (0.5 * syI.length); // head

                const Evec3 centerJ = ECmap3(syNearJ.pos);
                const Evec3 directionJ = ECmap3(syNearJ.direction);
                const Evec3 Qm = centerJ - directionJ * (0.5 * syNearJ.length); // tail
                // const Evec3 Qp = centerJ + directionJ * (0.5 * syJ.length); // head
                Evec3 Ploc = Pp; // head of I is linked to tail of J
                Evec3 Qloc = Qm;
                const Evec3 vecIJ = Ploc - Qloc;
                const double dist = vecIJ.norm();
                const Evec3 normI = vecIJ / dist;
                const Evec3 normJ = -normI;
                const Evec3 posI = Ploc - centerI;
                const Evec3 posJ = Qloc - centerJ;
                const double sep = dist - (syI.radius + syNearJ.radius) * 1.05; // L - L0
                double gamma = -sep * kappa;
                block = ConstraintBlock(sep, gamma,           // L - L0, initial guess of gamma
                                        syI.gid, syNearJ.gid, //
                                        syI.globalIndex,      //
                                        syNearJ.globalIndex,  //
                                        normI, normJ,         // direction of collision force
                                        posI, posJ,           // location of collision relative to particle center
                                        Ploc, Qloc,           // location of collision in lab frame
                                        false, kappa);
                Emat3 stressIJ;
                CalcSylinderNearForce::collideStress(directionI, directionJ, centerI, centerJ, syI.length,
                                                     syNearJ.length, syI.radius, syNearJ.radius, 1.0, Ploc, Qloc,
                                                     stressIJ);
                block.setStress(stressIJ);
                findIndex++;
            }
        }
    }
}