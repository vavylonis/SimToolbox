#ifndef PARTICLESYSTEM_HPP_
#define PARTICLESYSTEM_HPP_

#include "Particle/Particle.hpp"
#include "SystemConfig.hpp"

#include "Boundary/Boundary.hpp"
#include "Constraint/ConstraintBlock.hpp"
#include "Constraint/ConstraintSolver.hpp"
#include "Trilinos/TpetraUtil.hpp"
#include "Util/EigenDef.hpp"
#include "Util/TRngPool.hpp"

#include <msgpack.hpp>

#include <array>
#include <memory>
#include <unordered_map>
#include <utility>

template <class ParticleBase>
class ParticleSystem {
private:
  long stepID; ///< timestep id. sequentially numbered from 0

  std::shared_ptr<const SystemConfig> configPtr;

  // TRngPool object for thread-safe random number generation
  std::shared_ptr<TRngPool> rngPoolPtr;
  // Teuchos::MpiComm
  Teuchos::RCP<const TCOMM> commRcp;

  // pointer to ConstraintSolver
  std::shared_ptr<ConstraintSolver> conSolverPtr;
  // pointer to ConstraintCollector
  std::shared_ptr<ConstraintCollector> conCollectorPtr;

  /*********************************************
   *
   *   These are particle basic data
   *
   **********************************************/
  std::vector<ParticleBase> particles;
  Teuchos::RCP<TMAP> ptclMapRcp;     ///< TMAP, contiguous and sequentially
                                     ///< ordered 1 dof per sylinder
  Teuchos::RCP<TMAP> ptclMobMapRcp;  ///< TMAP, contiguous and sequentially
                                     ///< ordered 6 dofs per sylinder
  Teuchos::RCP<TCMAT> ptclMobMatRcp; ///< block-diagonal mobility matrix
  Teuchos::RCP<TOP> ptclMobOpRcp;    ///< full mobility operator
                                     ///< (matrix-free), to be implemented

  /*********************************************
   *
   *   These are computed BEFORE the constraint solver
   *
   **********************************************/
  Teuchos::RCP<TV> forcePartNonConRcp;
  Teuchos::RCP<TV> velPartNonConRcp;
  Teuchos::RCP<TV> velNonConRcp;

  // Brownian velocity
  Teuchos::RCP<TV> velBrownRcp;

  // velTotalNonCon = velNonCon + velBrown
  Teuchos::RCP<TV> velTotalNonConRcp;

  /*********************************************
   *
   *   These are computed by the constraint solver
   *
   **********************************************/
  Teuchos::RCP<TV> forceConURcp; ///< unilateral constraint force
  Teuchos::RCP<TV> forceConBRcp; ///< bilateral constraint force
  Teuchos::RCP<TV> velConURcp;   ///< unilateral constraint velocity
  Teuchos::RCP<TV> velConBRcp;   ///< bilateral constraint velocity

public:
  ParticleSystem() = default;
  ~ParticleSystem() = default;

  ParticleSystem(const ParticleSystem &) = delete;
  ParticleSystem(ParticleSystem &&) = delete;
  ParticleSystem &operator=(const ParticleSystem &) = delete;
  ParticleSystem &operator=(ParticleSystem &&) = delete;

  /*********************************************
   *
   *   member getters
   *
   **********************************************/

  auto getStepID() { return stepID; }
  auto getConfig() { return configPtr; }
  auto getRngPool() { return rngPoolPtr; }
  auto getComm() { return commRcp; }

  auto getConSolver() { return conSolverPtr; }
  auto getConCollector() { return conCollectorPtr; }

  auto getPtclContainer() { return particles; }
  auto getPtclMap() { return ptclMapRcp; }
  auto getPtclMobMap() { return ptclMobMapRcp; }
  auto getPtclMobMat() { return ptclMobMatRcp; }
  auto getPtclMobOp() { return ptclMobOpRcp; }

  auto getForcePartNonCon() { return forcePartNonConRcp; }
  auto getVelPartNonCon() { return velPartNonConRcp; }
  auto getVelNonCon() { return velNonConRcp; }
  auto getVelBrown() { return velBrownRcp; }
  auto getVelTotalNonCon() { return velTotalNonConRcp; }

  auto getForceConU() { return forceConURcp; }
  auto getForceConB() { return forceConBRcp; }
  auto getVelConU() { return velConURcp; }
  auto getVelConB() { return velConBRcp; }

  /*********************************************
   *
   *   member setters
   *
   **********************************************/
  void setForcePartNonCon(const std::vector<double> &forcePartNonCon) {
    const int nLocal = particles.size();
    TEUCHOS_ASSERT(ptclMapRcp->getNodeNumElements() == nLocal);
    TEUCHOS_ASSERT(ptclMobMapRcp->getNodeNumElements() == 6 * nLocal);
    forcePartNonConRcp = getTVFromVector(forcePartNonCon, commRcp);
  }

  void setVelPartNonCon(const std::vector<double> &forcePartNonCon) {
    const int nLocal = particles.size();
    TEUCHOS_ASSERT(ptclMapRcp->getNodeNumElements() == nLocal);
    TEUCHOS_ASSERT(ptclMobMapRcp->getNodeNumElements() == 6 * nLocal);
    velPartNonConRcp = getTVFromVector(velPartNonCon, commRcp);
  }

  /*********************************************
   *
   *   calculations
   *
   **********************************************/

  /**
   * @brief calculate total non-constraint velocity
   *
   *  \f$U_{TotalNonCon} = U_{Brown} + U_{Part,NonCon} + M_{UF}
   * F_{Part,NonCon}\f$
   */
  void calcVelTotalNonCon() {
    const int nLocal = particles.size();
    TEUCHOS_ASSERT(ptclMobMapRcp->getNodeNumElements() == 6 * nLocal);
    TEUCHOS_ASSERT(!velTotalNonConRcp.is_null());
    TEUCHOS_ASSERT(velTotalNonConRcp->getLocalLength() == 6 * nLocal);
    velTotalNonConRcp->putScalar(0.0);

    // step 1, force part
    if (!forcePartNonConRcp.is_null()) {
      TEUCHOS_ASSERT(!ptclMobOpRcp.is_null());
      TEUCHOS_ASSERT(forcePartNonConRcp->getLocalLength() == 6 * nLocal);
      // U_{TotalNonCon} = M_{UF} F_{Part,NonCon}
      ptclMobOpRcp->apply(*forcePartNonConRcp, *velTotalNonConRcp);
      auto forceView = forcePartNonConRcp->getLocalView<Kokkos::HostSpace>();
#pragma omp parallel for
      for (int i = 0; i < nLocal; i++) {
        auto &par = particles[i];
        for (int j = 0; j < 6; j++) {
          par.forceNonCon[j] = forceView(6 * i + j, 0);
        }
      }
    }

    // step 2, vel part
    if (!velPartNonConRcp.is_null()) {
      TEUCHOS_ASSERT(velPartNonConRcp->getLocalLength() == 6 * nLocal);
      // U_{TotalNonCon} += U_{Part,NonCon}
      velTotalNonConRcp->update(1.0, *velPartNonConRcp, 1.0);
    }

    // step3, write back to particle data
    auto velTotalView = velTotalNonConRcp->getLocalView<Kokkos::HostSpace>();
#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
      auto &par = particles[i];
      for (int j = 0; j < 6; j++) {
        par.velNonCon[j] = velTotalView(6 * i + j, 0);
      }
    }

    // step4, velBrown
    if (!velBrownRcp.is_null()) {
      TEUCHOS_ASSERT(velBrownRcp->getLocalLength() == 6 * nLocal);
      // U_{TotalNonCon} += U_{Brown}
      velTotalNonConRcp->update(1.0, *velBrownRcp, 1.0);
    }

    // step 5, monolayer
    if (configPtr->monolayer) {
#pragma omp parallel for
      for (int i = 0; i < nLocal; i++) {
        velTotalView(6 * i + 2, 0) = 0; // vz
        velTotalView(6 * i + 3, 0) = 0; // wx
        velTotalView(6 * i + 4, 0) = 0; // wy
      }
    }
  }

  /**
   * @brief update the mpi rank for particles
   *
   */
  void updatePtclRank() {
    const int nLocal = sylinderContainer.getNumberOfParticleLocal();
    const int rank = commRcp->getRank();
#pragma omp parallel for
    for (long i = 0; i < nLocal; i++) {
      particles[i].rank = rank;
    }
  }

  /**
   * @brief update the particle map and particle mobility map
   *
   * This function is called in prepareStep(),
   * and no adding/removing/exchanging is allowed before runStep()
   */
  void updatePtclMap() {
    const int nLocal = particles.size();
    ptclMapRcp = getTMAPFromLocalSize(nLocal, commRcp);
    ptclMobMapRcp = getTMAPFromLocalSize(nLocal * 6, commRcp);

    // setup the globalIndex
    long globalIndexBase =
        ptclMapRcp->getMinGlobalIndex(); // this is a contiguous map
#pragma omp parallel for
    for (int i = 0; i < nLocal; i++) {
      particles[i].globalIndex = i + globalIndexBase;
    }
  }

  /**
   * @brief apply periodic boundary condition
   *
   */
  void applyBoxPBC() { return; }

  /**
   * @brief resolve collision with given nonBrownian motion and advance the
   * system configuration
   *
   */
  void runStep() {}

  void initialize(std::shared_ptr<const SystemConfig> &configPtr_,
                  const std::string &posFile, int argc, char **argv) {
    commRcp = Tpetra::getDefaultComm();
    stepID = 0;

    configPtr = configPtr_;
    if (!commRcp->getRank()) {
      configPtr->echo();
    }

    Logger::set_level(configPtr->logLevel);

    // TRNG pool must be initialized after mpi is initialized
    rngPoolPtr = std::make_shared<TRngPool>(configPtr->rngSeed);
    conSolverPtr = std::make_shared<ConstraintSolver>();
    conCollectorPtr = std::make_shared<ConstraintCollector>();

    particles.clear();
    particles.reserve(1000);

    if (IOHelper::fileExist(posFile)) {
      // at this point all sylinders located on rank 0
      readFromDatFile(posFile);
    }

    if (commRcp->getRank() == 0) {
      IOHelper::makeSubFolder("./result"); // prepare the output directory
      writeBox();
    }

    spdlog::warn("ParticleSystem Initialized. {} local particles",
                 particles.size());
  }

  /*********************************************
   *
   *   read write
   *
   **********************************************/

  /**
   * @brief write a simple legacy VTK file for simBox
   *
   */
  void writeBox() const {
    FILE *boxFile = fopen("./result/simBox.vtk", "w");
    fprintf(boxFile, "# vtk DataFile Version 3.0\n");
    fprintf(boxFile, "vtk file\n");
    fprintf(boxFile, "ASCII\n");
    fprintf(boxFile, "DATASET RECTILINEAR_GRID\n");
    fprintf(boxFile, "DIMENSIONS 2 2 2\n");
    fprintf(boxFile, "X_COORDINATES 2 float\n");
    fprintf(boxFile, "%g %g\n", configPtr->simBoxLow[0],
            configPtr->simBoxHigh[0]);
    fprintf(boxFile, "Y_COORDINATES 2 float\n");
    fprintf(boxFile, "%g %g\n", configPtr->imBoxLow[1],
            configPtr->simBoxHigh[1]);
    fprintf(boxFile, "Z_COORDINATES 2 float\n");
    fprintf(boxFile, "%g %g\n", configPtr->simBoxLow[2],
            configPtr->simBoxHigh[2]);
    fprintf(boxFile, "CELL_DATA 1\n");
    fprintf(boxFile, "POINT_DATA 8\n");
    fclose(boxFile);
  }

  /**
   * @brief read ascii dat file to rank 0
   *
   * @param datFile
   */
  void readFromDatFile(const std::string &datFile) {
    spdlog::warn("Reading file " + filename);

    if (!commRcp->getRank()) {
      std::string line;
      particles.clear();

      std::ifstream myfile(datFile);
      while (std::getline(myfile, line)) {
        if (line[0] == '#') {
          // this is a comment line
          continue;
        } else {
          // parse a particle
          std::stringstream liness(line);
          particles.emplace_back(liness);
        }
      }
      myfile.close();
    }

    spdlog::debug("Particle number in file: {} ", particles.size());
  }

  void writeData() const {}

  /*********************************************
   *
   *   statistics
   *
   **********************************************/

  /**
   * @brief calculate particle volume
   *
   * @return double total particle volume
   */
  double calcPtclVol() const {
    double lclPtclVol = 0;
    const int nLocal = particles.size();
#pragma omp parallel for reduction(+ : lclPtclVol)
    for (int i = 0; i < nLocal; i++) {
      lclPtclVol += particles[i].getVolume();
    }

    double glbPtclVol = lclPtclVol;
    MPI_Allreduce(MPI_IN_PLACE, lclPtclVol, 1, MPI_DOUBLE, MPI_SUM,
                  MPI_COMM_WORLD);

    return glbPtclVol;
  }

  std::array<double, 9> calcStressConB() const {
    std::array<double, 9> stress;
    return stress;
  }

  std::array<double, 9> calcStressConU() const {
    std::array<double, 9> stress;
    return stress;
  }

  std::array<double, 3> calcPolarity() const {
    std::array<double, 3> polarity;
    return polarity
  }

  /*********************************************
   *
   *   print info
   *
   **********************************************/

  void printTimingSummary(const bool zeroOut) {
    if (configPtr->timerLevel <= spdlog::level::info)
      Teuchos::TimeMonitor::summarize();
    if (zeroOut)
      Teuchos::TimeMonitor::zeroOutTimers();
  }

  /**
   * @brief display the configuration on rank 0
   *
   */
  void echo() const {
    if (commRcp->getRank() == 0) {
      for (const auto &p : particles) {
        p.echo();
      }
    }
  }
};

#endif