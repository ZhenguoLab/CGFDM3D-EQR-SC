# ================================================================
#   ESS, Southern University of Science and Technology
#
#   File Name: verifyTerrain.py
#   Author: Wenqiang Wang, 11849528@mail.sustech.edu.cn
#           Tianhong Xu, 12231218@mail.sustech.edu.cn
#   Created Time: 2021-9-12
#   Discription: Verify the output terrain
#
#   Reference:
#      1. Wang, W., Zhang, Z., Zhang, W., Yu, H., Liu, Q., Zhang, W., & Chen, X. (2022). CGFDM3D‐EQR: A platform for rapid response to earthquake disasters in 3D complex media. Seismological Research Letters, 93(4), 2320-2334. https://doi.org/https://doi.org/10.1785/0220210172
#      2. Xu, T., & Zhang, Z. (2024). Numerical simulation of 3D seismic wave based on alternative flux finite-difference WENO scheme. Geophysical Journal International, 238(1), 496-512. https://doi.org/https://doi.org/10.1093/gji/ggae167
# 
# =================================================================*/

import json
import numpy as np
from pyscripts.GRID import GRID
import matplotlib.pyplot as plt


jsonsFile = open("params.json")
params = json.load(jsonsFile)
grid = GRID(params)
FAST_AXIS = params["FAST_AXIS"]

sample = 1

outputPath = params["out"]

lonFileName = params["out"] + "/lon"
latFileName = params["out"] + "/lat"
terrainFileName = params["out"] + "/terrain"

sliceX = params["sliceX"] - grid.frontNX
sliceY = params["sliceY"] - grid.frontNY
sliceZ = params["sliceZ"] - grid.frontNZ


if FAST_AXIS == 'Z':
    lon = np.zeros([grid.NX, grid.NY])
    lat = np.zeros([grid.NX, grid.NY])
    terrain = np.zeros([grid.NX, grid.NY])
else:
    lon = np.zeros([grid.NY, grid.NX])
    lat = np.zeros([grid.NY, grid.NX])
    terrain = np.zeros([grid.NY, grid.NX])


mpiZ = grid.PZ - 1
for mpiY in range(grid.PY):
    for mpiX in range(grid.PX):
        lonFile = open("%s_mpi_%d_%d_%d.bin" %
                       (lonFileName, mpiX, mpiY, mpiZ), "rb")
        latFile = open("%s_mpi_%d_%d_%d.bin" %
                       (latFileName, mpiX, mpiY, mpiZ), "rb")
        terrainFile = open("%s_mpi_%d_%d_%d.bin" %
                           (terrainFileName, mpiX, mpiY, mpiZ), "rb")

        ny = grid.ny[mpiY]
        nx = grid.nx[mpiX]

        print("ny = %d, nx = %d" % (nx, ny))
        lon_ = np.fromfile(lonFile, dtype='float32', count=ny * nx)
        lat_ = np.fromfile(latFile, dtype='float32', count=ny * nx)
        terrain_ = np.fromfile(terrainFile, dtype='float32', count=ny * nx)

        J = grid.frontNY[mpiY]
        J_ = grid.frontNY[mpiY] + ny
        I = grid.frontNX[mpiX]
        I_ = grid.frontNX[mpiX] + nx

        if FAST_AXIS == 'Z':
            lon[I:I_, J:J_] = np.reshape(lon_, 		(nx, ny))
            lat[I:I_, J:J_] = np.reshape(lat_, 		(nx, ny))
            terrain[I:I_, J:J_] = np.reshape(terrain_, 	(nx, ny))
        else:
            lon[J:J_, I:I_] = np.reshape(lon_, (ny, nx))
            lat[J:J_, I:I_] = np.reshape(lat_, (ny, nx))
            terrain[J:J_, I:I_] = np.reshape(terrain_, (ny, nx))


dpi = 300
fig = plt.figure(dpi=dpi, figsize=(1920 // dpi, 1080 // dpi))
unit = 1000  # 1km = 1000m


plt.pcolor(lon[::sample, ::sample], lat[::sample, ::sample],
           terrain[::sample, ::sample], cmap="jet")
plt.axis("image")
plt.colorbar()
plt.savefig("./img/terrain.png")

# plt.plot( dataZ[grid.NZ - 1, :] )

np.savetxt("./data/terrain.txt", np.c_[lon.flatten(), lat.flatten(),
                                terrain.flatten()], fmt='%.3f', delimiter=' ')
