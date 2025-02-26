// Include Files
//==============

#include "Graphics.h"

#if defined( EAE6320_PLATFORM_GL )
#include "OpenGL\Includes.h"
#endif

#if defined(EAE6320_PLATFORM_WINDOWS)
#include "Direct3D\Includes.h" 
#endif

#include "cConstantBuffer.h"
#include "ConstantBufferFormats.h"
#include "cRenderState.h"
#include "cSamplerState.h"
#include "cShader.h"
#include "cTexture.h"
#include "cMesh.h"
#include "sContext.h"
#include "VertexFormats.h"
#include "Engine\Graphics\cEffect.h"
#include "Engine\Graphics\cSprite.h"
#include "cView.h"

#include <vector>
#include <algorithm>
#include <Engine/Asserts/Asserts.h>
#include <Engine/Concurrency/cEvent.h>
#include <Engine/Logging/Logging.h>
#include <Engine/Platform/Platform.h>
#include <Engine/Time/Time.h>
#include <Engine/UserOutput/UserOutput.h>
#include <Engine/Math/cMatrix_transformation.h>
#include <Engine/Physics/sRigidBodyState.h>
#include <utility>

// Static Data Initialization
//===========================

namespace
{

	// Constant buffer object
	eae6320::Graphics::cConstantBuffer s_constantBuffer_perFrame(eae6320::Graphics::ConstantBufferTypes::PerFrame);
	eae6320::Graphics::cConstantBuffer s_constantBuffer_perDraw(eae6320::Graphics::ConstantBufferTypes::PerDrawCall);
	// In our class we will only have a single sampler state
	eae6320::Graphics::cSamplerState s_samplerState;

	// Submission Data
	//----------------

	// This struct's data is populated at submission time;
	// it must cache whatever is necessary in order to render a frame
	struct sDataRequiredToRenderAFrame
	{
		eae6320::Graphics::ConstantBufferFormats::sPerFrame constantData_perFrame;
		eae6320::Graphics::ConstantBufferFormats::sPerDrawCall constantData_perDraw;
		float backgroundColor[4];
		std::vector<eae6320::Graphics::renderData> renderDataVec;
		std::vector<eae6320::Graphics::meshData> meshDataVec;
		std::vector<eae6320::Graphics::meshData> meshTranslucentDataVec;

	};
	// In our class there will be two copies of the data required to render a frame:
	//	* One of them will be getting populated by the data currently being submitted by the application loop thread
	//	* One of them will be fully populated, 
	sDataRequiredToRenderAFrame s_dataRequiredToRenderAFrame[2];
	auto* s_dataBeingSubmittedByApplicationThread = &s_dataRequiredToRenderAFrame[0];
	auto* s_dataBeingRenderedByRenderThread = &s_dataRequiredToRenderAFrame[1];
	// The following two events work together to make sure that
	// the main/render thread and the application loop thread can work in parallel but stay in sync:
	// This event is signaled by the application loop thread when it has finished submitting render data for a frame
	// (the main/render thread waits for the signal)
	eae6320::Concurrency::cEvent s_whenAllDataHasBeenSubmittedFromApplicationThread;
	// This event is signaled by the main/render thread when it has swapped render data pointers.
	// This means that the renderer is now working with all the submitted data it needs to render the next frame,
	// and the application loop thread can start submitting data for the following frame
	// (the application loop thread waits for the signal)
	eae6320::Concurrency::cEvent s_whenDataForANewFrameCanBeSubmittedFromApplicationThread;

	cView view;

}

// Submission
//-----------

void eae6320::Graphics::SubmitElapsedTime(const float i_elapsedSecondCount_systemTime, const float i_elapsedSecondCount_simulationTime)
{
	EAE6320_ASSERT(s_dataBeingSubmittedByApplicationThread);
	auto& constantData_perFrame = s_dataBeingSubmittedByApplicationThread->constantData_perFrame;
	//auto& constantData_perDraw = s_dataBeingSubmittedByApplicationThread->constantData_perDraw;

	constantData_perFrame.g_elapsedSecondCount_systemTime = i_elapsedSecondCount_systemTime;
	constantData_perFrame.g_elapsedSecondCount_simulationTime = i_elapsedSecondCount_simulationTime;

}

// put it in the order of RGB A
void eae6320::Graphics::SubmitBackgroundColor(const float r, const float g, const float b, const float a) {
	s_dataBeingSubmittedByApplicationThread->backgroundColor[0] = r;
	s_dataBeingSubmittedByApplicationThread->backgroundColor[1] = g;
	s_dataBeingSubmittedByApplicationThread->backgroundColor[2] = b;
	s_dataBeingSubmittedByApplicationThread->backgroundColor[3] = a;
}
void eae6320::Graphics::SubmitEffectAndSprite(eae6320::Graphics::renderData data)
{
	data.effect->IncrementReferenceCount();
	data.sprite->IncrementReferenceCount();
	data.texture->IncrementReferenceCount();
	
	s_dataBeingSubmittedByApplicationThread->renderDataVec.push_back(data);
}

void eae6320::Graphics::SubmitEffectAndMesh(eae6320::Graphics::meshData & data, eae6320::Physics::sRigidBodyState & rigidBodyState)
{
	EAE6320_ASSERT(s_dataBeingSubmittedByApplicationThread);
	auto& constantData_perFrame = s_dataBeingSubmittedByApplicationThread->constantData_perFrame;

	data.effect->IncrementReferenceCount();
	data.mesh->IncrementReferenceCount();
	data.texture->IncrementReferenceCount();

	data.rigidBodyState.orientation = rigidBodyState.PredictFutureOrientation(constantData_perFrame.g_elapsedSecondCount_simulationTime);
	data.rigidBodyState.position = rigidBodyState.PredictFuturePosition(constantData_perFrame.g_elapsedSecondCount_simulationTime);

	// for translucent meshes
	if (data.effect->s_renderState.IsAlphaTransparencyEnabled()) {
		s_dataBeingSubmittedByApplicationThread->meshTranslucentDataVec.push_back(data);
	}
	// for opaque meshes
	else {
		s_dataBeingSubmittedByApplicationThread->meshDataVec.push_back(data);
	}
}

void eae6320::Graphics::SubmitCamera(eae6320::Graphics::cCamera & camera) {
	EAE6320_ASSERT(s_dataBeingSubmittedByApplicationThread);
	auto& constantData_perFrame = s_dataBeingSubmittedByApplicationThread->constantData_perFrame;

	//eae6320::Physics::sRigidBodyState rigidBodyState = camera.m_rigidBodyState;
	eae6320::Physics::sRigidBodyState rigidBodyState;
    rigidBodyState.orientation = camera.m_rigidBodyState.PredictFutureOrientation(constantData_perFrame.g_elapsedSecondCount_simulationTime);
	rigidBodyState.position = camera.m_rigidBodyState.PredictFuturePosition(constantData_perFrame.g_elapsedSecondCount_simulationTime);

	constantData_perFrame.g_transform_worldToCamera = eae6320::Math::cMatrix_transformation::CreateWorldToCameraTransform(
		/*camera.m_rigidBodyState.orientation,
		camera.m_rigidBodyState.position);*/
		rigidBodyState.orientation,
		rigidBodyState.position);

	constantData_perFrame.g_transform_cameraToProjected =
		eae6320::Math::cMatrix_transformation::CreateCameraToProjectedTransform_perspective(
			camera.m_verticalFieldOfView_inRadians, 
			camera.m_aspectRatio,
			camera.m_z_nearPlane, 
			camera.m_z_farPlane);

	//eae6320::Math::cQuaternion futureOrientation = rigidBodyState.PredictFutureOrientation(constantData_perFrame.g_elapsedSecondCount_simulationTime);
	//eae6320::Math::sVector futurePosition = rigidBodyState.PredictFuturePosition(constantData_perFrame.g_elapsedSecondCount_simulationTime);
}

eae6320::cResult eae6320::Graphics::WaitUntilDataForANewFrameCanBeSubmitted(const unsigned int i_timeToWait_inMilliseconds)
{
	return Concurrency::WaitForEvent(s_whenDataForANewFrameCanBeSubmittedFromApplicationThread, i_timeToWait_inMilliseconds);
}

eae6320::cResult eae6320::Graphics::SignalThatAllDataForAFrameHasBeenSubmitted()
{
	return s_whenAllDataHasBeenSubmittedFromApplicationThread.Signal();
}

// Render
//-------

void eae6320::Graphics::RenderFrame()
{
	// Wait for the application loop to submit data to be rendered
	{
		const auto result = Concurrency::WaitForEvent(s_whenAllDataHasBeenSubmittedFromApplicationThread);
		if (result)
		{
			// Switch the render data pointers so that
			// the data that the application just submitted becomes the data that will now be rendered
			std::swap(s_dataBeingSubmittedByApplicationThread, s_dataBeingRenderedByRenderThread);
			// Once the pointers have been swapped the application loop can submit new data
			const auto result = s_whenDataForANewFrameCanBeSubmittedFromApplicationThread.Signal();
			if (!result)
			{
				EAE6320_ASSERTF(false, "Couldn't signal that new graphics data can be submitted");
				Logging::OutputError("Failed to signal that new render data can be submitted");
				UserOutput::Print("The renderer failed to signal to the application that new graphics data can be submitted."
					" The application is probably in a bad state and should be exited");
				return;
			}
		}
		else
		{
			EAE6320_ASSERTF(false, "Waiting for the graphics data to be submitted failed");
			Logging::OutputError("Waiting for the application loop to submit data to be rendered failed");
			UserOutput::Print("The renderer failed to wait for the application to submit data to be rendered."
				" The application is probably in a bad state and should be exited");
			return;
		}
	}
	

	EAE6320_ASSERT(s_dataBeingRenderedByRenderThread);

	view.Clear(s_dataBeingRenderedByRenderThread->backgroundColor[0],
		s_dataBeingRenderedByRenderThread->backgroundColor[1],
		s_dataBeingRenderedByRenderThread->backgroundColor[2],
		s_dataBeingRenderedByRenderThread->backgroundColor[3]);

	// Update the per-frame constant buffer
	{
		// Copy the data from the system memory that the application owns to GPU memory
		auto& constantData_perFrame = s_dataBeingRenderedByRenderThread->constantData_perFrame;

		s_constantBuffer_perFrame.Update(&constantData_perFrame);
	}

	// draw all the opaque meshes first
	for (auto data : s_dataBeingRenderedByRenderThread->meshDataVec) {

		auto& constantData_perDraw = s_dataBeingRenderedByRenderThread->constantData_perDraw;

		constantData_perDraw.g_transform_localToWorld = eae6320::Math::cMatrix_transformation(
			data.rigidBodyState.orientation, data.rigidBodyState.position);

		s_constantBuffer_perDraw.Update(&constantData_perDraw);

		data.effect->Bind();
		data.texture->Bind(0);
		data.mesh->DrawMesh();
	}
	
	// sort the for all translucent meshes
	std::vector<std::pair<float, size_t>> translucentVecToSort;
	for (size_t i = 0; i < s_dataBeingRenderedByRenderThread->meshTranslucentDataVec.size(); i++) {
		auto& constantData_perDraw = s_dataBeingRenderedByRenderThread->constantData_perDraw;
		auto& constantData_perFrame = s_dataBeingRenderedByRenderThread->constantData_perFrame;

		auto& data = s_dataBeingRenderedByRenderThread->meshTranslucentDataVec[i];

		constantData_perDraw.g_transform_localToWorld = eae6320::Math::cMatrix_transformation(
			data.rigidBodyState.orientation, data.rigidBodyState.position);

		auto dataCameraSpaceTransform = constantData_perFrame.g_transform_worldToCamera * constantData_perDraw.g_transform_localToWorld;
		float dataZPosition = dataCameraSpaceTransform.GetTranslation().z;
		translucentVecToSort.push_back(std::make_pair(dataZPosition, i));
	}

	std::sort(translucentVecToSort.begin(), translucentVecToSort.end());

	for (size_t i = 0; i < translucentVecToSort.size(); i++) {
		auto& data = s_dataBeingRenderedByRenderThread->meshTranslucentDataVec[translucentVecToSort[i].second];

		auto& constantData_perDraw = s_dataBeingRenderedByRenderThread->constantData_perDraw;

		constantData_perDraw.g_transform_localToWorld = eae6320::Math::cMatrix_transformation(
			data.rigidBodyState.orientation, data.rigidBodyState.position);

		s_constantBuffer_perDraw.Update(&constantData_perDraw);

		data.effect->Bind();
		data.texture->Bind(0);
		data.mesh->DrawMesh();
	}

	for (auto data : s_dataBeingRenderedByRenderThread->renderDataVec) {
		data.effect->Bind();
		data.texture->Bind(0);
		data.sprite->Draw();
	}
	view.Buffer();
	// Once everything has been drawn the data that was submitted for this frame
	// should be cleaned up and cleared.
	// so that the struct can be re-used (i.e. so that data for a new frame can be submitted to it)
	{
		for (auto data : s_dataBeingRenderedByRenderThread->meshDataVec) {
			data.effect->DecrementReferenceCount();
			data.mesh->DecrementReferenceCount();
			data.texture->DecrementReferenceCount();
		}
		s_dataBeingRenderedByRenderThread->meshDataVec.clear();
		
		for (auto data : s_dataBeingRenderedByRenderThread->renderDataVec) {
			data.effect->DecrementReferenceCount();
			data.texture->DecrementReferenceCount();
			data.sprite->DecrementReferenceCount();
		}

		s_dataBeingRenderedByRenderThread->renderDataVec.clear();
	}
}

// Initialization / Clean Up
//--------------------------

eae6320::cResult eae6320::Graphics::Initialize(const sInitializationParameters& i_initializationParameters)
{
	auto result = Results::Success;

	// Initialize the platform-specific context
	if (!(result = sContext::g_context.Initialize(i_initializationParameters)))
	{
		EAE6320_ASSERT(false);
		goto OnExit;
	}
	// Initialize the asset managers
	{
		if (!(result = cShader::s_manager.Initialize()))
		{
			EAE6320_ASSERT(false);
			goto OnExit;
		}
	}

	// Initialize the platform-independent graphics objects
	{
		if (result = s_constantBuffer_perFrame.Initialize())
		{
			// There is only a single per-frame constant buffer that is re-used
			// and so it can be bound at initialization time and never unbound
			s_constantBuffer_perFrame.Bind(
				// In our class both vertex and fragment shaders use per-frame constant data
				ShaderTypes::Vertex | ShaderTypes::Fragment);
		}
		else
		{
			EAE6320_ASSERT(false);
			goto OnExit;
		}
		if (result = s_samplerState.Initialize())
		{
			// There is only a single sampler state that is re-used
			// and so it can be bound at initialization time and never unbound
			s_samplerState.Bind();
		}
		else
		{
			EAE6320_ASSERT(false);
			goto OnExit;
		}

		if (result = s_constantBuffer_perDraw.Initialize())
		{
			// There is only a single per-frame constant buffer that is re-used
			// and so it can be bound at initialization time and never unbound
			s_constantBuffer_perDraw.Bind(
				// In our class both vertex and fragment shaders use per-draw constant data
				ShaderTypes::Vertex | ShaderTypes::Fragment);
		}
		else
		{
			EAE6320_ASSERT(false);
			goto OnExit;
		}
	}

	// Initialize the events
	{
		if (!(result = s_whenAllDataHasBeenSubmittedFromApplicationThread.Initialize(Concurrency::EventType::ResetAutomaticallyAfterBeingSignaled)))
		{
			EAE6320_ASSERT(false);
			goto OnExit;
		}
		if (!(result = s_whenDataForANewFrameCanBeSubmittedFromApplicationThread.Initialize(Concurrency::EventType::ResetAutomaticallyAfterBeingSignaled,
			Concurrency::EventState::Signaled)))
		{
			EAE6320_ASSERT(false);
			goto OnExit;
		}
	}

#if defined(EAE6320_PLATFORM_D3D)
	// Initialize the views
	{
		if (!(result = view.InitializeViews(i_initializationParameters.resolutionWidth, i_initializationParameters.resolutionHeight)))
		{
			EAE6320_ASSERT(false);
			goto OnExit;
		}
	}
#endif

OnExit:

	return result;
}

eae6320::cResult eae6320::Graphics::CleanUp()
{
	auto result = Results::Success;

	result = view.CleanUp();

	
	for (auto data : s_dataBeingRenderedByRenderThread->renderDataVec) {
		data.effect->DecrementReferenceCount();
		data.texture->DecrementReferenceCount();
		data.sprite->DecrementReferenceCount();
	}
	s_dataBeingRenderedByRenderThread->renderDataVec.clear();

	for (auto data : s_dataBeingSubmittedByApplicationThread->renderDataVec) {
		data.effect->DecrementReferenceCount();
		data.texture->DecrementReferenceCount();
		data.sprite->DecrementReferenceCount();
	}

	s_dataBeingSubmittedByApplicationThread->renderDataVec.clear();

	for (auto data : s_dataBeingRenderedByRenderThread->meshDataVec) {
		data.effect->DecrementReferenceCount();
		data.texture->DecrementReferenceCount();
		data.mesh->DecrementReferenceCount();
	}
	s_dataBeingRenderedByRenderThread->meshDataVec.clear();

	for (auto data : s_dataBeingSubmittedByApplicationThread->meshDataVec) {
		data.effect->DecrementReferenceCount();
		data.mesh->DecrementReferenceCount();
		data.texture->DecrementReferenceCount();
	}

	s_dataBeingSubmittedByApplicationThread->meshDataVec.clear();

	for (auto data : s_dataBeingRenderedByRenderThread->meshTranslucentDataVec) {
		data.effect->DecrementReferenceCount();
		data.texture->DecrementReferenceCount();
		data.mesh->DecrementReferenceCount();
	}
	s_dataBeingRenderedByRenderThread->meshTranslucentDataVec.clear();

	for (auto data : s_dataBeingSubmittedByApplicationThread->meshTranslucentDataVec) {
		data.effect->DecrementReferenceCount();
		data.mesh->DecrementReferenceCount();
		data.texture->DecrementReferenceCount();
	}

	s_dataBeingSubmittedByApplicationThread->meshTranslucentDataVec.clear();

	{
		const auto localResult = s_constantBuffer_perFrame.CleanUp();
		if (!localResult)
		{
			EAE6320_ASSERT(false);
			if (result)
			{
				result = localResult;
			}
		}
	}

	{
		const auto localResult = s_constantBuffer_perDraw.CleanUp();
		if (!localResult)
		{
			EAE6320_ASSERT(false);
			if (result)
			{
				result = localResult;
			}
		}
	}

	{
		const auto localResult = s_samplerState.CleanUp();
		if (!localResult)
		{
			EAE6320_ASSERT(false);
			if (result)
			{
				result = localResult;
			}
		}
	}

	{
		const auto localResult = cShader::s_manager.CleanUp();
		if (!localResult)
		{
			EAE6320_ASSERT(false);
			if (result)
			{
				result = localResult;
			}
		}
	}

	{
		const auto localResult = sContext::g_context.CleanUp();
		if (!localResult)
		{
			EAE6320_ASSERT(false);
			if (result)
			{
				result = localResult;
			}
		}
	}

	return result;
}

