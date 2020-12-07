/**********************************************************************************************************************
# Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
# following conditions are met:
#  * Redistributions of code must retain the copyright notice, this list of conditions and the following disclaimer.
#  * Neither the name of NVIDIA CORPORATION nor the names of its contributors may be used to endorse or promote products
#    derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT
# SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
# OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************************************************************/

#include "./SVGFShadowPass.h"

// TODO: on compile, allocate FBOs based on size

namespace {
	// Where is our shader located?
	const char kPackLinearZAndNormalShader[] = "SVGFShadow\\SVGFPackLinearZAndNormal.ps.hlsl";
	const char kReprojectShader[] = "SVGFShadow\\SVGFReproject.ps.hlsl";
	const char kAtrousShader[] = "SVGFShadow\\SVGFAtrous.ps.hlsl";
	const char kFilterMomentShader[] = "SVGFShadow\\SVGFFilterMoments.ps.hlsl";
	const char kFinalModulateShader[] = "SVGFShadow\\SVGFFinalModulate.ps.hlsl";

	// Input buffers
	const char kInputBufferAlbedo[] = "MaterialDiffuse";
	const char kInputBufferEmission[] = "MaterialEmissive";
	const char kInputBufferWorldPosition[] = "WorldPosition";
	const char kInputBufferWorldNormal[] = "WorldNormal";
	const char kInputBufferPosNormalFwidth[] = "PosNormalFWidth";
	const char kInputBufferLinearZ[] = "LinearZAndDeriv";
	const char kInputBufferMotionVector[] = "MotiveVectors";

	// Internal buffer names
	const char kInternalBufferPreviousLinearZAndNormal[] = "Shadow Previous Linear Z and Packed Normal";
	const char kInternalBufferPreviousLighting[] = "Shadow Previous Lighting";
	const char kInternalBufferPreviousMoments[] = "Shadow Previous Moments";
};

// Define our constructor methods
SVGFShadowPass::SharedPtr SVGFShadowPass::create(const std::string& bufferOut, const std::string &inputColorBuffer)
{
	return SharedPtr(new SVGFShadowPass(bufferOut, inputColorBuffer));
}

SVGFShadowPass::SVGFShadowPass(const std::string& bufferOut, const std::string &inputColorBuffer)
	: ::RenderPass("SVGF Shadow Pass", "SVGF Shadow Options")
{
	  mOutputTexName = bufferOut;
    mInputTexName = inputColorBuffer;
}

bool SVGFShadowPass::initialize(RenderContext* pRenderContext, ResourceManager::SharedPtr pResManager)
{
	// Stash our resource manager; ask for the texture the developer asked us to write
	mpResManager = pResManager;
	
	 mpResManager->requestTextureResources({
		kInputBufferEmission,
		kInputBufferWorldPosition,
		kInputBufferWorldNormal,
		kInputBufferPosNormalFwidth,
		kInputBufferLinearZ,
		kInputBufferMotionVector
	});

	mpResManager->requestTextureResources({
		kInternalBufferPreviousLinearZAndNormal,
		kInternalBufferPreviousLighting,
		kInternalBufferPreviousMoments
	});
	mpResManager->requestTextureResource(mOutputTexName);

	// Set the default scene to load
	//mpResManager->setDefaultSceneName("Data/pink_room/pink_room.fscene");

	// Create our graphics state and an accumulation shader
	mpGfxState = GraphicsState::create();
	
	mpPackLinearZAndNormal = FullscreenLaunch::create(kPackLinearZAndNormalShader);
	mpReprojection = FullscreenLaunch::create(kReprojectShader);
	mpAtrous = FullscreenLaunch::create(kAtrousShader);
	mpFilterMoments = FullscreenLaunch::create(kFilterMomentShader);
	mpFinalModulate = FullscreenLaunch::create(kFinalModulateShader);
	return true;
}

void SVGFShadowPass::allocateFbos(glm::uvec2 dim)
{
  {
    // Screen-size FBOs with 3 MRTs: one that is RGBA32F, one that is
    // RG32F for the luminance moments, and one that is R16F.
    Fbo::Desc desc;
    desc.setSampleCount(0);
    desc.setColorTarget(0, Falcor::ResourceFormat::RGBA32Float); // illumination
    desc.setColorTarget(1, Falcor::ResourceFormat::RG32Float);   // moments
    desc.setColorTarget(2, Falcor::ResourceFormat::R16Float);    // history length
	  mpCurReprojFbo = FboHelper::create2D(dim.x, dim.y, desc);
    mpPrevReprojFbo = FboHelper::create2D(dim.x, dim.y, desc);
  }

  {
    // Screen-size RGBA32F buffer for linear Z, derivative, and packed normal
    Fbo::Desc desc;
    desc.setColorTarget(0, Falcor::ResourceFormat::RGBA32Float);
    mpLinearZAndNormalFbo = FboHelper::create2D(dim.x, dim.y, desc);
  }

  {
    // Screen-size FBOs with 1 RGBA32F buffer
    Fbo::Desc desc;
    desc.setColorTarget(0, Falcor::ResourceFormat::RGBA32Float);
    mpPingPongFbo[0]  = FboHelper::create2D(dim.x, dim.y, desc);
    mpPingPongFbo[1]  = FboHelper::create2D(dim.x, dim.y, desc);
    mpFilteredPastFbo = FboHelper::create2D(dim.x, dim.y, desc);
    mpFilteredIlluminationFbo       = FboHelper::create2D(dim.x, dim.y, desc);
    mpFinalFbo        = FboHelper::create2D(dim.x, dim.y, desc);
  }

  mBuffersNeedClear = true;
}

void SVGFShadowPass::initScene(RenderContext* pRenderContext, Scene::SharedPtr pScene)
{

	// // When our renderer moves around we want to reset accumulation, so stash the scene pointer
	// mpScene = std::dynamic_pointer_cast<RtScene>(pScene);
	// if (!mpScene) return;
	// mpLambertShader->setLights(mpScene->getLights());
}

void SVGFShadowPass::resize(uint32_t width, uint32_t height)
{
	  glm::uvec2 dim(width, height);
    allocateFbos(dim);
}


void SVGFShadowPass::clearBuffers(RenderContext* pRenderContext) {
  pRenderContext->clearFbo(mpPingPongFbo[0].get(), float4(0), 1.0f, 0, FboAttachmentType::All);
  pRenderContext->clearFbo(mpPingPongFbo[1].get(), float4(0), 1.0f, 0, FboAttachmentType::All);
  pRenderContext->clearFbo(mpLinearZAndNormalFbo.get(), float4(0), 1.0f, 0, FboAttachmentType::All);
  pRenderContext->clearFbo(mpFilteredPastFbo.get(), float4(0), 1.0f, 0, FboAttachmentType::All);
  pRenderContext->clearFbo(mpCurReprojFbo.get(), float4(0), 1.0f, 0, FboAttachmentType::All);
  pRenderContext->clearFbo(mpPrevReprojFbo.get(), float4(0), 1.0f, 0, FboAttachmentType::All);
  pRenderContext->clearFbo(mpFilteredIlluminationFbo.get(), float4(0), 1.0f, 0, FboAttachmentType::All);

	mpResManager->clearTexture(mpResManager->getTexture(kInternalBufferPreviousLinearZAndNormal), glm::vec4(0.f));
	mpResManager->clearTexture(mpResManager->getTexture(kInternalBufferPreviousLighting), glm::vec4(0.f));
	mpResManager->clearTexture(mpResManager->getTexture(kInternalBufferPreviousMoments), glm::vec4(0.f));
}

void SVGFShadowPass::renderGui(Gui* pGui)
{
  int dirty = 0;
  dirty |= (int)pGui->addCheckBox(mFilterEnabled ? "SVGF enabled" : "SVGF disabled", mFilterEnabled);

  pGui->addText("");
  pGui->addText("Number of filter iterations.  Which");
  pGui->addText("    iteration feeds into future frames?");
  dirty |= (int)pGui->addIntVar("Iterations", mFilterIterations, 2, 10, 1);
  dirty |= (int)pGui->addIntVar("Feedback", mFeedbackTap, -1, mFilterIterations - 2, 1);

  pGui->addText("");
  pGui->addText("Contol edge stopping on bilateral fitler");
  dirty |= (int)pGui->addFloatVar("For Color", mPhiColor, 0.0f, 10000.0f, 0.01f);
  dirty |= (int)pGui->addFloatVar("For Normal", mPhiNormal, 0.001f, 1000.0f, 0.2f);

  pGui->addText("");
  pGui->addText("How much history should be used?");
  pGui->addText("    (alpha; 0 = full reuse; 1 = no reuse)");
  dirty |= (int)pGui->addFloatVar("Alpha", mAlpha, 0.0f, 1.0f, 0.001f);
  dirty |= (int)pGui->addFloatVar("Moments Alpha", mMomentsAlpha, 0.0f, 1.0f, 0.001f);

  if (dirty) mBuffersNeedClear = true;
}

void SVGFShadowPass::execute(RenderContext* pRenderContext)
{
	// Grab the texture to write to
	Texture::SharedPtr pColorTexture = mpResManager->getTexture(mInputTexName);
  Texture::SharedPtr pAlbedoTexture = mpResManager->getTexture(kInputBufferAlbedo);
	Texture::SharedPtr pEmissionTexture = mpResManager->getTexture(kInputBufferEmission);
	Texture::SharedPtr pWorldPositionTexture = mpResManager->getTexture(kInputBufferWorldPosition);
	Texture::SharedPtr pWorldNormalTexture = mpResManager->getTexture(kInputBufferWorldNormal);
	Texture::SharedPtr pPosNormalFwidthTexture = mpResManager->getTexture(kInputBufferPosNormalFwidth);
	Texture::SharedPtr pLinearZTexture = mpResManager->getTexture(kInputBufferLinearZ);
	Texture::SharedPtr pMotionVectorTexture = mpResManager->getTexture(kInputBufferMotionVector);
	Texture::SharedPtr pOutputTexture = mpResManager->getTexture(mOutputTexName);

	// If our input texture is invalid, or we've been asked to skip accumulation, do nothing.
	if (!pOutputTexture) return;

	if (mBuffersNeedClear) {
		clearBuffers(pRenderContext);
		mBuffersNeedClear = false;
	}

	if (!mFilterEnabled) {
		pRenderContext->blit(pColorTexture->getSRV(), pOutputTexture->getRTV());
		return;
	}
  // Grab linear z and its derivative and also pack the normal into
  // the last two channels of the mpLinearZAndNormalFbo.
  computeLinearZAndNormal(pRenderContext, pLinearZTexture, pWorldNormalTexture);
  
  // Demodulate input color & albedo to get illumination and lerp in
  // reprojected filtered illumination from the previous frame.
  // Stores the result as well as initial moments and an updated
  // per-pixel history length in mpCurReprojFbo.
  Texture::SharedPtr pPrevLinearZAndNormalTexture = mpResManager->getTexture(kInternalBufferPreviousLinearZAndNormal);
  computeReprojection(pRenderContext, pAlbedoTexture, pColorTexture, pEmissionTexture,
                            pMotionVectorTexture, pPosNormalFwidthTexture,
                            pPrevLinearZAndNormalTexture);
  
  // Do a first cross-bilateral filtering of the illumination and
  // estimate its variance, storing the result into a float4 in
  // mpPingPongFbo[0].  Takes mpCurReprojFbo as input.
  computeFilteredMoments(pRenderContext);
  
  // Filter illumination from mpCurReprojFbo[0], storing the result
  // in mpPingPongFbo[0].  Along the way (or at the end, depending on
  // the value of mFeedbackTap), save the filtered illumination for
  // next time into mpFilteredPastFbo.
  computeAtrousDecomposition(pRenderContext, pAlbedoTexture);


	// Compute albedo * filtered illumination and add emission back in.
  auto shaderVars = mpFinalModulate->getVars();
  // shaderVars["gAlbedo"] = pAlbedoTexture;
  shaderVars["gEmission"] = pEmissionTexture;
  shaderVars["gIllumination"] = mpPingPongFbo[0]->getColorTexture(0);
  mpGfxState->setFbo(mpFinalFbo);
  mpFinalModulate->execute(pRenderContext, mpGfxState);

  // Blit into the output texture.
  pRenderContext->blit(mpFinalFbo->getColorTexture(0)->getSRV(), pOutputTexture->getRTV());

  // Swap resources so we're ready for next frame.
  std::swap(mpCurReprojFbo, mpPrevReprojFbo);
  pRenderContext->blit(mpLinearZAndNormalFbo->getColorTexture(0)->getSRV(),
                        pPrevLinearZAndNormalTexture->getRTV());
}

void SVGFShadowPass::computeLinearZAndNormal(RenderContext* pRenderContext, Texture::SharedPtr pLinearZTexture,
                                       Texture::SharedPtr pWorldNormalTexture)
{
  auto shaderVars = mpPackLinearZAndNormal->getVars();
  shaderVars["gLinearZ"] = pLinearZTexture;
  shaderVars["gNormal"] = pWorldNormalTexture;
  mpGfxState->setFbo(mpLinearZAndNormalFbo);
  mpPackLinearZAndNormal->execute(pRenderContext, mpGfxState);
}

void SVGFShadowPass::computeReprojection(RenderContext* pRenderContext, Texture::SharedPtr pAlbedoTexture,
                                   Texture::SharedPtr pColorTexture, Texture::SharedPtr pEmissionTexture,
                                   Texture::SharedPtr pMotionVectorTexture,
                                   Texture::SharedPtr pPositionNormalFwidthTexture,
                                   Texture::SharedPtr pPrevLinearZTexture)
{
  auto shaderVars = mpReprojection->getVars();

  // Setup textures for our reprojection shader pass
  shaderVars["gMotion"]        = pMotionVectorTexture;
  shaderVars["gColor"]         = pColorTexture;
  shaderVars["gEmission"]      = pEmissionTexture;
  // shaderVars["gAlbedo"]        = pAlbedoTexture;
  shaderVars["gPositionNormalFwidth"] = pPositionNormalFwidthTexture;
  shaderVars["gPrevIllum"]     = mpFilteredPastFbo->getColorTexture(0);
  shaderVars["gPrevMoments"]   = mpPrevReprojFbo->getColorTexture(1);
  shaderVars["gLinearZAndNormal"]       = mpLinearZAndNormalFbo->getColorTexture(0);
  shaderVars["gPrevLinearZAndNormal"]   = pPrevLinearZTexture;
  shaderVars["gPrevHistoryLength"] = mpPrevReprojFbo->getColorTexture(2);

  // Setup variables for our reprojection pass
  shaderVars["PerImageCB"]["gAlpha"] = mAlpha;
  shaderVars["PerImageCB"]["gMomentsAlpha"] = mMomentsAlpha;

  mpGfxState->setFbo(mpCurReprojFbo);
  mpReprojection->execute(pRenderContext, mpGfxState);
}

void SVGFShadowPass::computeFilteredMoments(RenderContext* pRenderContext)
{
  auto shaderVars = mpFilterMoments->getVars();

  shaderVars["gIllumination"]     = mpCurReprojFbo->getColorTexture(0);
  shaderVars["gMoments"]          = mpCurReprojFbo->getColorTexture(1);
  shaderVars["gHistoryLength"]    = mpCurReprojFbo->getColorTexture(2);
  shaderVars["gLinearZAndNormal"]          = mpLinearZAndNormalFbo->getColorTexture(0);

  shaderVars["PerImageCB"]["gPhiColor"]  = mPhiColor;
  shaderVars["PerImageCB"]["gPhiNormal"]  = mPhiNormal;

  mpGfxState->setFbo(mpPingPongFbo[0]);
  mpFilterMoments->execute(pRenderContext, mpGfxState);
}


void SVGFShadowPass::computeAtrousDecomposition(RenderContext* pRenderContext, Texture::SharedPtr pAlbedoTexture)
{
  auto shaderVars = mpAtrous->getVars();
  // shaderVars["gAlbedo"]        = pAlbedoTexture;
  shaderVars["gHistoryLength"] = mpCurReprojFbo->getColorTexture(2);
  shaderVars["gLinearZAndNormal"]       = mpLinearZAndNormalFbo->getColorTexture(0);
  shaderVars["PerImageCB"]["gPhiColor"]  = mPhiColor;
  shaderVars["PerImageCB"]["gPhiNormal"] = mPhiNormal;

  for (int i = 0; i < mFilterIterations; i++)
  {
    Fbo::SharedPtr curTargetFbo = mpPingPongFbo[1];
    shaderVars["gIllumination"] = mpPingPongFbo[0]->getColorTexture(0);
    shaderVars["PerImageCB"]["gStepSize"] = 1 << i;
    mpGfxState->setFbo(curTargetFbo);
    mpAtrous->execute(pRenderContext, mpGfxState);

    // store the filtered color for the feedback path
    if (i == std::min(mFeedbackTap, mFilterIterations - 1))
    {
        pRenderContext->blit(curTargetFbo->getColorTexture(0)->getSRV(), mpFilteredPastFbo->getRenderTargetView(0));
    }

    std::swap(mpPingPongFbo[0], mpPingPongFbo[1]);
  }

  if (mFeedbackTap < 0)
  {
    pRenderContext->blit(mpCurReprojFbo->getColorTexture(0)->getSRV(), mpFilteredPastFbo->getRenderTargetView(0));
  }
}