#pragma once

#ifndef _SOLVER_EQUATIONS_LIE_
#define _SOLVER_EQUATIONS_LIE_


#include "GlobalDefines.h"
#ifdef USE_LIE_SPACE

#define THREADS_PER_BLOCK_JT 128

#include <cutil_inline.h>
#include <cutil_math.h>

#include "../SiftGPU/cuda_SimpleMatrixUtil.h"

#include "SolverBundlingUtil.h"
#include "SolverBundlingState.h"
#include "SolverBundlingParameters.h"

#include "LieDerivUtil.h"

// residual functions only for sparse!

// not squared!
__inline__ __device__ float evalAbsResidualDeviceFloat3(unsigned int corrIdx, unsigned int componentIdx, SolverInput& input, SolverState& state, SolverParameters& parameters)
{
	float3 r = make_float3(0.0f, 0.0f, 0.0f);

	const EntryJ& corr = input.d_correspondences[corrIdx];
	if (corr.isValid()) {
		float4x4 TI = poseToMatrix(state.d_xRot[corr.imgIdx_i], state.d_xTrans[corr.imgIdx_i]);
		float4x4 TJ = poseToMatrix(state.d_xRot[corr.imgIdx_j], state.d_xTrans[corr.imgIdx_j]);

		r = parameters.weightSparse * fabs((TI*corr.pos_i) - (TJ*corr.pos_j));
		if (componentIdx == 0) return r.x;
		if (componentIdx == 1) return r.y;
		return r.z; //if (componentIdx == 2) 
	}
	return 0.0f;
}

__inline__ __device__ float evalFDevice(unsigned int corrIdx, SolverInput& input, SolverState& state, SolverParameters& parameters)
{
	float3 r = make_float3(0.0f, 0.0f, 0.0f);

	const EntryJ& corr = input.d_correspondences[corrIdx];
	if (corr.isValid()) {
		float4x4 TI = poseToMatrix(state.d_xRot[corr.imgIdx_i], state.d_xTrans[corr.imgIdx_i]);
		float4x4 TJ = poseToMatrix(state.d_xRot[corr.imgIdx_j], state.d_xTrans[corr.imgIdx_j]);

		r = (TI*corr.pos_i) - (TJ*corr.pos_j);

		float res = parameters.weightSparse * dot(r, r);
		return res;
	}
	return 0.0f;
}

////////////////////////////////////////
// applyJT : this function is called per variable and evaluates each residual influencing that variable (i.e., each energy term per variable)
////////////////////////////////////////

__inline__ __device__ void evalMinusJTFDevice(unsigned int variableIdx, SolverInput& input, SolverState& state, SolverParameters& parameters, float3& resRot, float3& resTrans)
{
	float3 rRot = make_float3(0.0f, 0.0f, 0.0f);
	float3 rTrans = make_float3(0.0f, 0.0f, 0.0f);

	float3 pRot = make_float3(0.0f, 0.0f, 0.0f);
	float3 pTrans = make_float3(0.0f, 0.0f, 0.0f);

	// Reset linearized update vector
	state.d_deltaRot[variableIdx] = make_float3(0.0f, 0.0f, 0.0f);
	state.d_deltaTrans[variableIdx] = make_float3(0.0f, 0.0f, 0.0f);

	// Compute -JTF here
	int N = min(input.d_numEntriesPerRow[variableIdx], input.maxCorrPerImage);

	for (int i = 0; i < N; i++)
	{
		int corrIdx = input.d_variablesToCorrespondences[variableIdx*input.maxCorrPerImage + i];
		const EntryJ& corr = input.d_correspondences[corrIdx];
		if (corr.isValid()) {
			const float4x4 TI = poseToMatrix(state.d_xRot[corr.imgIdx_i], state.d_xTrans[corr.imgIdx_i]); //TODO precompute this at the beginnning of each iteration??
			const float4x4 TJ = poseToMatrix(state.d_xRot[corr.imgIdx_j], state.d_xTrans[corr.imgIdx_j]);

			float3 worldP; float variableSign = 1;
			if (variableIdx != corr.imgIdx_i)
			{
				variableSign = -1;
				worldP = TJ * corr.pos_j;
			}
			else {
				worldP = TI * corr.pos_i;
			}
			const float3 da = evalLie_dAlpha(worldP); //d(e) * T * p
			const float3 db = evalLie_dBeta(worldP);
			const float3 dc = evalLie_dGamma(worldP);

			const float3 r = (TI*corr.pos_i) - (TJ*corr.pos_j);

			rRot += variableSign * make_float3(dot(da, r), dot(db, r), dot(dc, r));
			rTrans += variableSign * r; 

			pRot += make_float3(dot(da, da), dot(db, db), dot(dc, dc));
			pTrans += make_float3(1.0f, 1.0f, 1.0f);
		}
	}
	resRot = -parameters.weightSparse * rRot;
	resTrans = -parameters.weightSparse * rTrans;
	pRot *= parameters.weightSparse;
	pTrans *= parameters.weightSparse;

	// add dense term //TODO UNCOMMENT HERE
	//uint3 rotIndices = make_uint3(variableIdx * 6 + 0, variableIdx * 6 + 1, variableIdx * 6 + 2);
	//uint3 transIndices = make_uint3(variableIdx * 6 + 3, variableIdx * 6 + 4, variableIdx * 6 + 5);
	//resRot -= make_float3(state.d_denseJtr[rotIndices.x], state.d_denseJtr[rotIndices.y], state.d_denseJtr[rotIndices.z]); //minus since -Jtf, weight already built in
	//resTrans -= make_float3(state.d_denseJtr[transIndices.x], state.d_denseJtr[transIndices.y], state.d_denseJtr[transIndices.z]); //minus since -Jtf, weight already built in
	//// preconditioner
	//pRot += make_float3(
	//	state.d_denseJtJ[rotIndices.x * input.numberOfImages * 6 + rotIndices.x],
	//	state.d_denseJtJ[rotIndices.y * input.numberOfImages * 6 + rotIndices.y],
	//	state.d_denseJtJ[rotIndices.z * input.numberOfImages * 6 + rotIndices.z]);
	//pTrans += make_float3(
	//	state.d_denseJtJ[transIndices.x * input.numberOfImages * 6 + transIndices.x],
	//	state.d_denseJtJ[transIndices.y * input.numberOfImages * 6 + transIndices.y],
	//	state.d_denseJtJ[transIndices.z * input.numberOfImages * 6 + transIndices.z]);
	// end dense part

	// Preconditioner depends on last solution P(input.d_x)
	if (pRot.x > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].x = 1.0f / pRot.x;
	else					      state.d_precondionerRot[variableIdx].x = 1.0f;

	if (pRot.y > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].y = 1.0f / pRot.y;
	else					      state.d_precondionerRot[variableIdx].y = 1.0f;

	if (pRot.z > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].z = 1.0f / pRot.z;
	else						  state.d_precondionerRot[variableIdx].z = 1.0f;

	if (pTrans.x > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].x = 1.0f / pTrans.x;
	else					      state.d_precondionerTrans[variableIdx].x = 1.0f;

	if (pTrans.y > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].y = 1.0f / pTrans.y;
	else					      state.d_precondionerTrans[variableIdx].y = 1.0f;

	if (pTrans.z > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].z = 1.0f / pTrans.z;
	else					      state.d_precondionerTrans[variableIdx].z = 1.0f;
}

////////////////////////////////////////
// applyJT : this function is called per variable and evaluates each residual influencing that variable (i.e., each energy term per variable)
////////////////////////////////////////

__inline__ __device__ void applyJTDevice(unsigned int variableIdx, SolverInput& input, SolverState& state, const SolverParameters& parameters,
	float3& outRot, float3& outTrans, unsigned int threadIdx, unsigned int lane)
{
	// Compute J^T*d_Jp here
	outRot = make_float3(0.0f, 0.0f, 0.0f);
	outTrans = make_float3(0.0f, 0.0f, 0.0f);

	int N = min(input.d_numEntriesPerRow[variableIdx], input.maxCorrPerImage);

	for (int i = threadIdx; i < N; i += THREADS_PER_BLOCK_JT)
	{
		int corrIdx = input.d_variablesToCorrespondences[variableIdx*input.maxCorrPerImage + i];
		const EntryJ& corr = input.d_correspondences[corrIdx];
		if (corr.isValid()) {
			const float4x4 TI = poseToMatrix(state.d_xRot[corr.imgIdx_i], state.d_xTrans[corr.imgIdx_i]); //TODO precompute this at the beginnning of each iteration??
			const float4x4 TJ = poseToMatrix(state.d_xRot[corr.imgIdx_j], state.d_xTrans[corr.imgIdx_j]);

			float3 worldP;
			float  variableSign = 1;
			if (variableIdx != corr.imgIdx_i)
			{
				variableSign = -1;
				worldP = TJ * corr.pos_j;
			}
			else {
				worldP = TI * corr.pos_i;
			}
			const float3 da = evalLie_dAlpha(worldP);
			const float3 db = evalLie_dBeta(worldP);
			const float3 dc = evalLie_dGamma(worldP);

			outRot += variableSign * make_float3(dot(da, state.d_Jp[corrIdx]), dot(db, state.d_Jp[corrIdx]), dot(dc, state.d_Jp[corrIdx]));
			outTrans += variableSign * state.d_Jp[corrIdx];
		}
	}
	//apply j already applied the weight

	outRot.x = warpReduce(outRot.x);	 outRot.y = warpReduce(outRot.y);	  outRot.z = warpReduce(outRot.z);
	outTrans.x = warpReduce(outTrans.x); outTrans.y = warpReduce(outTrans.y); outTrans.z = warpReduce(outTrans.z);
}

__inline__ __device__ float3 applyJDevice(unsigned int corrIdx, SolverInput& input, SolverState& state, const SolverParameters& parameters)
{
	// Compute Jp here
	float3 b = make_float3(0.0f, 0.0f, 0.0f);
	const EntryJ& corr = input.d_correspondences[corrIdx];

	if (corr.isValid()) {
		if (corr.imgIdx_i > 0)	// get transform 0
		{
			const float4x4 TI = poseToMatrix(state.d_xRot[corr.imgIdx_i], state.d_xTrans[corr.imgIdx_i]); //TODO precompute this at the beginnning of each iteration??
			const float3 worldP = TI * corr.pos_i;
			const float3 da = evalLie_dAlpha(worldP);
			const float3 db = evalLie_dBeta(worldP);
			const float3 dc = evalLie_dGamma(worldP);

			const float3  pp0 = state.d_pRot[corr.imgIdx_i];
			b += da*pp0.x + db*pp0.y + dc*pp0.z + state.d_pTrans[corr.imgIdx_i];
		}

		if (corr.imgIdx_j > 0)	// get transform 1
		{
			const float4x4 TJ = poseToMatrix(state.d_xRot[corr.imgIdx_j], state.d_xTrans[corr.imgIdx_j]); //TODO precompute this at the beginnning of each iteration??
			const float3 worldP = TJ * corr.pos_j;
			const float3 da = evalLie_dAlpha(worldP);
			const float3 db = evalLie_dBeta(worldP);
			const float3 dc = evalLie_dGamma(worldP);

			const float3  pp1 = state.d_pRot[corr.imgIdx_j];
			b -= da*pp1.x + db*pp1.y + dc*pp1.z + state.d_pTrans[corr.imgIdx_j];
		}
		b *= parameters.weightSparse;
	}
	return b;
}

////////////////////////////////////////
// dense depth term
////////////////////////////////////////

__inline__ __device__ void computeJacobianBlockRow_i(matNxM<1, 6>& jacBlockRow, const float3& angles, const float3& translation,
	const float4x4& transform_j, const float4& camPosSrc, const float4& normalTgt)
{
	////!!!DEBUGGING
	//if (isnan(camPosSrc.x) || isnan(camPosSrc.y) || isnan(camPosSrc.z) || isnan(camPosSrc.w)) {
	//	printf("ERROR jac i: camPosSrc = %f %f %f %f\n", camPosSrc.x, camPosSrc.y, camPosSrc.z, camPosSrc.w);
	//}
	//if (isnan(normalTgt.x) || isnan(normalTgt.y) || isnan(normalTgt.z) || isnan(normalTgt.w)) {
	//	printf("ERROR jac i: camPosSrc = %f %f %f %f\n", normalTgt.x, normalTgt.y, normalTgt.z, normalTgt.w);
	//}
	////!!!DEBUGGING

	//float4 world = transform_j * camPosSrc;
	////alpha
	//float4x4 dx = evalRtInverse_dAlpha(angles, translation);
	//jacBlockRow(0) = -dot(dx * world, normalTgt);
	////beta
	//dx = evalRtInverse_dBeta(angles, translation);
	//jacBlockRow(1) = -dot(dx * world, normalTgt);
	////gamma
	//dx = evalRtInverse_dGamma(angles, translation);
	//jacBlockRow(2) = -dot(dx * world, normalTgt);
	////x
	//dx = evalRtInverse_dX(angles, translation);
	//jacBlockRow(3) = -dot(dx * world, normalTgt);
	////y
	//dx = evalRtInverse_dY(angles, translation);
	//jacBlockRow(4) = -dot(dx * world, normalTgt);
	////z
	//dx = evalRtInverse_dZ(angles, translation);
	//jacBlockRow(5) = -dot(dx * world, normalTgt);
}
__inline__ __device__ void computeJacobianBlockRow_j(matNxM<1, 6>& jacBlockRow, const float3& angles, const float3& translation,
	const float4x4& invTransform_i, const float4& camPosSrc, const float4& normalTgt)
{
	////!!!DEBUGGING
	//if (isnan(camPosSrc.x) || isnan(camPosSrc.y) || isnan(camPosSrc.z) || isnan(camPosSrc.w)) {
	//	printf("ERROR jac j: camPosSrc = %f %f %f %f\n", camPosSrc.x, camPosSrc.y, camPosSrc.z, camPosSrc.w);
	//}
	//if (isnan(normalTgt.x) || isnan(normalTgt.y) || isnan(normalTgt.z) || isnan(normalTgt.w)) {
	//	printf("ERROR jac j: camPosSrc = %f %f %f %f\n", normalTgt.x, normalTgt.y, normalTgt.z, normalTgt.w);
	//}
	////!!!DEBUGGING

	//float4x4 dx; dx.setIdentity();
	////alpha
	//dx.setFloat3x3(evalR_dAlpha(angles));
	//jacBlockRow(0) = -dot(invTransform_i * dx * camPosSrc, normalTgt);
	////beta
	//dx.setFloat3x3(evalR_dBeta(angles));
	//jacBlockRow(1) = -dot(invTransform_i * dx * camPosSrc, normalTgt);
	////gamma
	//dx.setFloat3x3(evalR_dGamma(angles));
	//jacBlockRow(2) = -dot(invTransform_i * dx * camPosSrc, normalTgt);
	////x
	//float4 dt = make_float4(1.0f, 0.0f, 0.0f, 1.0f);
	//jacBlockRow(3) = -dot(invTransform_i * dt, normalTgt);
	////y
	//dt = make_float4(0.0f, 1.0f, 0.0f, 1.0f);
	//jacBlockRow(4) = -dot(invTransform_i * dt, normalTgt);
	////z
	//dt = make_float4(0.0f, 0.0f, 1.0f, 1.0f);
	//jacBlockRow(5) = -dot(invTransform_i * dt, normalTgt);
}
////////////////////////////////////////
// dense depth term
////////////////////////////////////////
__inline__ __device__ float computeColorDProjLookup(const float4& dx, const float4& camPosSrcToTgt, const float2& intensityDerivTgt, const float2& colorFocalLength)
{
	//mat3x1 dcdx; dcdx(0) = dx.x; dcdx(1) = dx.y; dcdx(2) = dx.z;
	//mat2x3 dProjectionC = dCameraToScreen(camPosSrcToTgt, colorFocalLength.x, colorFocalLength.y);
	//mat1x2 dColorB(intensityDerivTgt);
	//mat1x1 dadx = dColorB * dProjectionC * dcdx;

	//return dadx(0);
	return MINF;
}
__inline__ __device__ void computeJacobianBlockIntensityRow_i(matNxM<1, 6>& jacBlockRow, const float2& colorFocal, const float3& angles, const float3& translation,
	const float4x4& transform_j, const float4& camPosSrc, const float4& camPosSrcToTgt, const float2& intensityDerivTgt)
{
	//float4 world = transform_j * camPosSrc;
	////alpha
	//float4 dx = evalRtInverse_dAlpha(angles, translation) * world;
	//jacBlockRow(0) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////beta
	//dx = evalRtInverse_dBeta(angles, translation) * world;
	//jacBlockRow(1) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////gamma
	//dx = evalRtInverse_dGamma(angles, translation) * world;
	//jacBlockRow(2) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////x
	//dx = evalRtInverse_dX(angles, translation) * world;
	//jacBlockRow(3) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////y
	//dx = evalRtInverse_dY(angles, translation) * world;
	//jacBlockRow(4) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////z
	//dx = evalRtInverse_dZ(angles, translation) * world;
	//jacBlockRow(5) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
}
__inline__ __device__ void computeJacobianBlockIntensityRow_j(matNxM<1, 6>& jacBlockRow, const float2& colorFocal, const float3& angles, const float3& translation,
	const float4x4& invTransform_i, const float4& camPosSrc, const float4& camPosSrcToTgt, const float2& intensityDerivTgt)
{
	////alpha
	//float4 dx = invTransform_i * evalR_dAlpha(angles) * camPosSrc;
	//jacBlockRow(0) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////beta
	//dx = invTransform_i * evalR_dBeta(angles) * camPosSrc;
	//jacBlockRow(1) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////gamma
	//dx = invTransform_i * evalR_dGamma(angles) * camPosSrc;
	//jacBlockRow(2) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////x
	//dx = invTransform_i * make_float4(1.0f, 0.0f, 0.0f, 1.0f);
	//jacBlockRow(3) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////y
	//dx = invTransform_i * make_float4(0.0f, 1.0f, 0.0f, 1.0f);
	//jacBlockRow(4) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
	////z
	//dx = invTransform_i * make_float4(0.0f, 0.0f, 1.0f, 1.0f);
	//jacBlockRow(5) = computeColorDProjLookup(dx, camPosSrcToTgt, intensityDerivTgt, colorFocal);
}
#endif

#endif //_SOLVER_EQUATIONS_LIE_
