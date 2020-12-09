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

// This render pass is similar to the "SimpleDiffuseGIPass" from Tutorial #12, except it 
//     uses a more complex GGX illumination model (instead of a simple Lambertian model).
//     The changes mostly affect the HLSL shader code, as the C++ is quite similar

#pragma once
#include "../SharedUtils/RenderPass.h"
#include "../SharedUtils/RayLaunch.h"

class GlobalIlluminationPass : public ::RenderPass, inherit_shared_from_this<::RenderPass, GlobalIlluminationPass>
{
public:
    using SharedPtr = std::shared_ptr<GlobalIlluminationPass>;
    using SharedConstPtr = std::shared_ptr<const GlobalIlluminationPass>;

    static SharedPtr create(const std::string &outChannel) { return SharedPtr(new GlobalIlluminationPass(outChannel)); }
    virtual ~GlobalIlluminationPass() = default;

protected:
	GlobalIlluminationPass(const std::string &outChannel) : mOutputTextureName(outChannel),
		::RenderPass("Global Illum., GGX BRDF", "GGX Global Illumination Options") {}

    // Implementation of RenderPass interface
    bool initialize(RenderContext* pRenderContext, ResourceManager::SharedPtr pResManager) override;
    void initScene(RenderContext* pRenderContext, Scene::SharedPtr pScene) override;
    void execute(RenderContext* pRenderContext) override;
	void renderGui(Gui* pGui) override;

	// Override some functions that provide information to the RenderPipeline class
	bool requiresScene() override { return true; }
	bool usesRayTracing() override { return true; }

    // Rendering state
	RayLaunch::SharedPtr    mpRays;                       ///< Our wrapper around a DX Raytracing pass
    RtScene::SharedPtr      mpScene;                      ///< Our scene file (passed in from app)  

	// Recursive ray tracing can be slow.  Add a toggle to disable, to allow you to manipulate the scene
	bool                    mDoIndirectGI = true;
	bool                    mDoDirectGI = true;

	int32_t                 mUserSpecifiedRayDepth = 1;   ///<  What is the current maximum ray depth
	const int32_t           mMaxPossibleRayDepth = 8;     ///<  The largest ray depth we support (without recompile)
	bool                    mIsOpenScene = true;

	// What texture should was ask the resource manager to store our result in?
	std::string             mOutputTextureName;
    
	// Various internal parameters
	uint32_t                mFrameCount = 0x1337u;        ///< A frame counter to vary random numbers over time
};
