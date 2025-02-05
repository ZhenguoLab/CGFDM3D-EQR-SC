/*================================================================
*   ESS, Southern University of Science and Technology
*
*   File Name: propagate.cpp
*   Author: Wenqiang Wang, 11849528@mail.sustech.edu.cn
*   Created Time: 2021-11-03
*   Discription: Propagate the wave
*
*	Update: Tianhong Xu, 12231218@mail.sustech.edu.cn
*   Update Time: 2023-11-16
*   Update Content: Add SCFDM
*
*   Reference:
*      1. Wang, W., Zhang, Z., Zhang, W., Yu, H., Liu, Q., Zhang, W., & Chen, X. (2022). CGFDM3D‐EQR: A platform for rapid response to earthquake disasters in 3D complex media. Seismological Research Letters, 93(4), 2320-2334. https://doi.org/https://doi.org/10.1785/0220210172
*      2. Xu, T., & Zhang, Z. (2024). Numerical simulation of 3D seismic wave based on alternative flux finite-difference WENO scheme. Geophysical Journal International, 238(1), 496-512. https://doi.org/https://doi.org/10.1093/gji/ggae167
*
=================================================================*/

#include "header.h"
#ifdef SCFDM
#define RK_NUM 3
#else
#define RK_NUM 4
#endif

#ifdef SOLVE_PGA
__global__ void stroage_W_pre_gpu(FLOAT *W, FLOAT *W_pre, int _nx_, int _ny_, int _nz_)
{
	int i = threadIdx.x + blockIdx.x * blockDim.x;
	int j = threadIdx.y + blockIdx.y * blockDim.y;
	int k = threadIdx.z + blockIdx.z * blockDim.z;

	long long index = INDEX(i, j, k);

	W_pre[index] = W[index];
}

void storage_W_pre(GRID grid, WAVE wave)
{
	int _nx_ = grid._nx_;
	int _ny_ = grid._ny_;
	int _nz_ = grid._nz_;

#ifdef XFAST
	dim3 threads(32, 4, 4);
#endif
#ifdef ZFAST
	dim3 threads(1, 8, 64);
#endif

	dim3 blocks;
	blocks.x = (_nx_ + threads.x - 1) / threads.x;
	blocks.y = (_ny_ + threads.y - 1) / threads.y;
	blocks.z = (_nz_ + threads.z - 1) / threads.z;

	stroage_W_pre_gpu<<<blocks, threads>>>(wave.W, wave.W_pre, _nx_, _ny_, _nz_);
}
#endif

void isMPIBorder(GRID grid, MPI_COORD thisMPICoord, MPI_BORDER *border)
{

	if (0 == thisMPICoord.X)
		border->isx1 = 1;
	if ((grid.PX - 1) == thisMPICoord.X)
		border->isx2 = 1;
	if (0 == thisMPICoord.Y)
		border->isy1 = 1;
	if ((grid.PY - 1) == thisMPICoord.Y)
		border->isy2 = 1;
	if (0 == thisMPICoord.Z)
		border->isz1 = 1;
	if ((grid.PZ - 1) == thisMPICoord.Z)
		border->isz2 = 1;
}

void propagate(
	MPI_Comm comm_cart, MPI_COORD thisMPICoord, MPI_NEIGHBOR mpiNeighbor,
	GRID grid, PARAMS params,
	WAVE wave, Mat_rDZ mat_rDZ, FLOAT *CJM,
	SOURCE_FILE_INPUT src_in, long long *srcIndex, float *momentRate,
	float *momentRateSlice, SLICE slice, SLICE_DATA sliceData, SLICE_DATA sliceDataCpu)
{
	int thisRank;
	MPI_Comm_rank(MPI_COMM_WORLD, &thisRank);

	float DT = params.DT;
	int NT = params.TMAX / DT;
	float DH = grid.DH;

	int IT_SKIP = params.IT_SKIP;

	int sliceFreeSurf = params.sliceFreeSurf;

	SLICE freeSurfSlice;
	float *pgv, *cpuPgv;

	locateFreeSurfSlice(grid, &freeSurfSlice);
	SLICE_DATA freeSurfData, freeSurfDataCpu;

#ifdef SOLVE_DISPLACEMENT
	FLOAT *Dis;
	CHECK(Malloc((void **)&Dis, sizeof(FLOAT) * 3 * grid._nx_ * grid._ny_ * grid._nz_));
	CHECK(Memset(Dis, 0, sizeof(FLOAT) * 3 * grid._nx_ * grid._ny_ * grid._nz_));
#endif

	int IsFreeSurface = 0;
#ifdef FREE_SURFACE
	if (thisMPICoord.Z == grid.PZ - 1)
		IsFreeSurface = 1;
#endif
	if (IsFreeSurface)
	{
		if (sliceFreeSurf)
			allocSliceData(grid, freeSurfSlice, &freeSurfData, &freeSurfDataCpu);

		allocatePGV(grid, &pgv, &cpuPgv);
	}

	MPI_BORDER border = {0};
	isMPIBorder(grid, thisMPICoord, &border);

	AUX6SURF Aux6;
#ifdef PML
	allocPML(grid, &Aux6, border);

	PML_ALPHA pml_alpha;
	PML_BETA pml_beta;
	PML_D pml_d;

	allocPMLParameter(grid, &pml_alpha, &pml_beta, &pml_d);
	init_pml_parameter(params, grid, border, pml_alpha, pml_beta, pml_d);
#endif

	int stationNum;
	STATION station, cpu_station;
	stationNum = readStationIndex(grid);

	if (stationNum > 0)
	{
		allocStation(&station, &cpu_station, stationNum, NT);
		initStationIndex(grid, cpu_station);
		stationCPU2GPU(station, cpu_station, stationNum);
	}

	SOURCE S = {0}; //{ _nx_ / 2, _ny_ / 2, _nz_ / 2 };
	locateSource(params, grid, &S);

	int useMultiSource = params.useMultiSource;
	int useSingleSource_ricker = params.useSingleSource_ricker;
	int useSingleSource_double_couple = params.useSingleSource_double_couple;

	int it = 0, irk = 0;
	int FB1 = 0;
	int FB2 = 0;
	int FB3 = 0;

	int FB[8][3] =
		{
			{-1, -1, -1},
			{1, 1, -1},
			{1, 1, 1},
			{-1, -1, 1},
			{-1, 1, -1},
			{1, -1, -1},
			{1, -1, 1},
			{-1, 1, 1},
		}; // F = 1, B = -1

	// GaussField( grid, wave.W );
	// useSingleSource = 0;

	float *gaussFactor;
	int nGauss = 3;
	if (useMultiSource)
	{
		allocGaussFactor(&gaussFactor, nGauss);
		calculateMomentRate(src_in, CJM, momentRate, srcIndex, DH);
	}

	MPI_Barrier(comm_cart);
	long long midClock = clock(), stepClock = 0;

	SEND_RECV_DATA_FLOAT sr_wave;
	FLOAT_allocSendRecv(grid, mpiNeighbor, &sr_wave, WSIZE);
	for (it = 0; it < NT; it++)
	{
#ifdef SOLVE_PGA
		if (it % 10 == 0)
		{
			storage_W_pre(grid, wave);
		}
#endif

		FB1 = FB[it % 8][0];
		FB2 = FB[it % 8][1];
		FB3 = FB[it % 8][2];
		if (useSingleSource_ricker)
			loadPointSource_ricker(grid, S, wave.W, CJM, it, 0, DT, DH, params.rickerfc);
		
		if (useSingleSource_double_couple)
			loadPointSource_double_couple(grid, S, wave.W, CJM, it, 0, DT, DH, params.strike, params.dip, params.rake, params.Mw, params.duration);

		if (useMultiSource)
			addMomenteRate(grid, src_in, wave.W, srcIndex, momentRate, momentRateSlice, it, 0, DT, DH, gaussFactor, nGauss, IsFreeSurface
#ifdef SCFDM
						   ,
						   CJM
#endif
			);

		for (irk = 0; irk < RK_NUM; irk++)
		{
			MPI_Barrier(comm_cart);
			FLOAT_mpiSendRecv(comm_cart, mpiNeighbor, grid, wave.W, sr_wave, WSIZE);
#ifdef PML
#ifdef SCFDM
			// ! For alternative flux finite difference by Tianhong Xu
			waveDeriv_alternative_flux_FD(grid, wave, CJM, pml_beta, FB1, FB2, FB3, DT, thisMPICoord, params);
#else
			waveDeriv(grid, wave, CJM, pml_beta, FB1, FB2, FB3, DT);
#endif // SCFDM
			if (IsFreeSurface)
				freeSurfaceDeriv(grid, wave, CJM, mat_rDZ, pml_beta, FB1, FB2, FB3, DT);
			pmlDeriv(grid, wave, CJM, Aux6, pml_alpha, pml_beta, pml_d, border, FB1, FB2, FB3, DT);
			if (IsFreeSurface)
				pmlFreeSurfaceDeriv(grid, wave, CJM, Aux6, mat_rDZ, pml_d, border, FB1, FB2, DT);
#else // PML
#ifdef SCFDM
			// ! For alternative flux finite difference by Tianhong Xu
			waveDeriv_alternative_flux_FD(grid, wave, CJM, FB1, FB2, FB3, DT, thisMPICoord, params);
#else
			waveDeriv(grid, wave, CJM, FB1, FB2, FB3, DT);
			if (IsFreeSurface)
				freeSurfaceDeriv(grid, wave, CJM, mat_rDZ, FB1, FB2, FB3, DT);
#endif // SCFDM

#endif // PML

#ifdef SCFDM
			waveRk_tvd(grid, irk, wave);
#else
			waveRk(grid, irk, wave);
#endif
#ifdef PML
			pmlRk(grid, border, irk, Aux6);
#endif
			FB1 *= -1;
			FB2 *= -1;
			FB3 *= -1; // reverse
		} // for loop of irk: Range Kutta Four Step
#ifdef SCFDM
// ! For alternative flux finite difference by Tianhong Xu
#ifdef FREE_SURFACE
		if (IsFreeSurface)
			charfreeSurfaceDeriv(grid, wave, CJM, mat_rDZ, FB1, FB2, FB3, DT);
#endif // FREE_SURFACE
#endif // SCFDM

#ifdef EXP_DECAY
		expDecayLayers(grid, wave);
#endif // EXP_DECAY

#ifdef SOLVE_DISPLACEMENT
		SolveDisplacement(grid, wave.W, Dis, CJM, DT);
#endif // SOLVE_DISPLACEMENT

		if (stationNum > 0)
			storageStation(grid, NT, stationNum, station, wave.W, it
#ifdef SCFDM
						   ,
						   CJM
#endif
			);
		if (IsFreeSurface)
			comparePGV(grid, thisMPICoord, wave.W, pgv, DT, it
#ifdef SCFDM
					   ,
					   CJM
#endif

#ifdef SOLVE_PGA
					   ,
					   wave.W_pre

#endif
			);

		if (it % IT_SKIP == 0)
		{
			data2D_XYZ_out(thisMPICoord, params, grid, wave.W, slice, sliceData, sliceDataCpu, 'V', it
#ifdef SCFDM
						   ,
						   CJM
#endif
			);
			if (IsFreeSurface && sliceFreeSurf)
			{
				data2D_XYZ_out(thisMPICoord, params, grid, wave.W, freeSurfSlice, freeSurfData, freeSurfDataCpu, 'F', it
#ifdef SCFDM
							   ,
							   CJM
#endif
				);
#ifdef SOLVE_DISPLACEMENT
				data2D_XYZ_out_Dis(thisMPICoord, params, grid, freeSurfSlice, freeSurfData, freeSurfDataCpu, it, Dis);
#endif
			}
		}
		/*
		 */
		MPI_Barrier(comm_cart);
		if ((0 == thisRank) && (it % 10 == 0))
		{
			printf("it = %8d. ", it);
			stepClock = clock() - midClock;
			midClock = stepClock + midClock;
			printf("Step time loss: %8.3lfs. Total time loss: %8.3lfs.\n", stepClock * 1.0 / (CLOCKS_PER_SEC * 1.0), midClock * 1.0 / (CLOCKS_PER_SEC * 1.0));
		}

	} // for loop of it: The time iterator of NT steps
	if (useMultiSource)
		freeGaussFactor(gaussFactor);

#ifdef PML
	freePML(border, Aux6);
	freePMLParamter(pml_alpha, pml_beta, pml_d);
#endif

	FLOAT_freeSendRecv(mpiNeighbor, sr_wave);
	MPI_Barrier(comm_cart);
	if (stationNum > 0)
	{
		stationGPU2CPU(station, cpu_station, stationNum, NT);
		stationWrite(params, grid, thisMPICoord, cpu_station, NT, stationNum);
		freeStation(station, cpu_station);
	}

	if (IsFreeSurface)
	{
		outputPGV(params, grid, thisMPICoord, pgv, cpuPgv);
		freePGV(pgv, cpuPgv);
		if (sliceFreeSurf)
			freeSliceData(grid, freeSurfSlice, freeSurfData, freeSurfDataCpu);
	}

#ifdef SOLVE_DISPLACEMENT
	Free(Dis);
#endif
}
