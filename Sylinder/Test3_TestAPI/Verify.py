import sys
import vtk
import glob
import re
import numpy as np
import scipy as sp
import yaml

# member variables are dynamically added by parsing data files

file = open('RunConfig.yaml')
config = yaml.load(file, Loader=yaml.FullLoader)
file.close()
print(config['linkKappa'])
linkKappa = config['linkKappa']


class ConBlock(object):
    end0 = None
    end1 = None
    pass


class Frame:

    def __init__(self, conBlockFile=None):
        self.conBlocks = []
        self.parseConBlockFile(conBlockFile)

    def parseFile(self, dataFile, objType, objList):
        # print("Parsing data from " + dataFile)
        # create vtk reader
        reader = vtk.vtkXMLPPolyDataReader()
        reader.SetFileName(dataFile)
        reader.Update()
        data = reader.GetOutput()

        # fill data
        # step 1, end coordinates
        nObj = int(data.GetPoints().GetNumberOfPoints() / 2)
        # print("parsing data for ", nObj, " sylinders")
        for i in range(nObj):
            s = objType()
            s.end0 = data.GetPoints().GetPoint(2 * i)
            s.end1 = data.GetPoints().GetPoint(2 * i + 1)
            objList.append(s)

        # step 2, member cell data
        numCellData = data.GetCellData().GetNumberOfArrays()
        # print("Number of CellDataArrays: ", numCellData)
        for i in range(numCellData):
            cdata = data.GetCellData().GetArray(i)
            dataName = cdata.GetName()
            # print("Parsing Cell Data", dataName)
            for j in range(len(objList)):
                setattr(objList[j], dataName, cdata.GetTuple(j))

        # step 3, member point data
        numPointData = data.GetPointData().GetNumberOfArrays()
        # print("Number of PointDataArrays: ", numPointData)
        for i in range(numPointData):
            pdata = data.GetPointData().GetArray(i)
            dataName = pdata.GetName()
            # print("Parsing Point Data", dataName)
            for j in range(len(objList)):
                setattr(objList[j], dataName + "0", pdata.GetTuple(2 * j))
                setattr(objList[j], dataName + "1", pdata.GetTuple(2 * j + 1))

        # output all data for debug
        # for s in objList[:10]:
        #     # print(s.end0, s.end1)
        #     attrs = vars(s)
        #     print('*************************************')
        #     print('\n'.join("%s: %s" % item for item in attrs.items()))
        #     print('*************************************')

        # print("-------------------------------------")
        return

    def parseConBlockFile(self, conBlockFile):
        self.parseFile(conBlockFile, ConBlock, self.conBlocks)
        self.conBlocks.sort(key=lambda x: (
            x.bilateral[0], x.gid0[0], x.gid1[0]))


def getFrameNumber_lambda(filename): return int(
    re.search('_([^_.]+)(?:\.[^_]*)?$', filename).group(1))


def check_link(frame):
    # 4 procs, each proc has 1 chain of 20 segments so 19 bi constraints
    # block 0 : 0,1
    # block 1 : 1,2
    # block 2 : 2,3
    # .......

    biblocks = []
    for b in frame.conBlocks:
        if b.bilateral[0] > 0:
            biblocks.append(b)
    for i in range(4):
        for j in range(18):
            a = biblocks[19*i+j].gid0[0]
            b = biblocks[19*i+j+1].gid1[0]
            print(a, b)
            if a != b:
                print("Fail : link error")


# get file list
# sort as numerical order
ConBlockFileList = glob.glob(
    './result/result*/ConBlock_*.pvtp')
ConBlockFileList.sort(key=getFrameNumber_lambda)

# example
frames = []
for i in range(len(ConBlockFileList)):
    # get frame
    frames.append(Frame(ConBlockFileList[i]))

for frame in frames[11:20]:
    check_link(frame)
