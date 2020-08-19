//
// Copyright (c) 2017-2020 the rbfx project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Precompiled.h"

#include "../Core/Context.h"
#include "../Core/WorkQueue.h"
/*
#include "../Core/IteratorRange.h"
#include "../Core/WorkQueue.h"
#include "../Math/Polyhedron.h"
#include "../Graphics/Camera.h"*/
#include "../Graphics/Renderer.h"
#include "../Graphics/ScenePass.h"
#include "../Graphics/Technique.h"
/*#include "../Graphics/Octree.h"
#include "../Graphics/OctreeQuery.h"
#include "../Graphics/Renderer.h"
#include "../Scene/Node.h"*/

#include "../DebugNew.h"

namespace Urho3D
{

ScenePass::ScenePass(Context* context,
    unsigned unlitBasePassIndex, unsigned litBasePassIndex, unsigned lightPassIndex)
    : Object(context)
    , workQueue_(context_->GetWorkQueue())
    , renderer_(context_->GetRenderer())
    , unlitBasePassIndex_(unlitBasePassIndex)
    , litBasePassIndex_(litBasePassIndex)
    , lightPassIndex_(lightPassIndex)
{
}

void ScenePass::BeginFrame()
{
    numThreads_ = workQueue_->GetNumThreads() + 1;

    unlitBatches_.Clear(numThreads_);
    litBatches_.Clear(numThreads_);

    unlitBaseBatchesDirty_.Clear(numThreads_);
    litBaseBatchesDirty_.Clear(numThreads_);
    lightBatchesDirty_.Clear(numThreads_);

    unlitBaseBatches_.clear();
    litBaseBatches_.clear();
    lightBatches_.Clear(numThreads_);
}

bool ScenePass::AddSourceBatch(Drawable* drawable, unsigned sourceBatchIndex, Technique* technique)
{
    const unsigned workerThreadIndex = WorkQueue::GetWorkerThreadIndex();

    Pass* unlitBasePass = technique->GetPass(unlitBasePassIndex_);
    Pass* litBasePass = technique->GetPass(litBasePassIndex_);
    Pass* lightPass = technique->GetPass(lightPassIndex_);

    if (lightPass)
    {
        if (litBasePass)
        {
            // Add normal lit batch
            litBatches_.Insert(workerThreadIndex, { drawable, sourceBatchIndex, litBasePass, lightPass });
            return true;
        }
        else if (unlitBasePass)
        {
            // Add both unlit and lit batches if there's no lit base
            unlitBatches_.Insert(workerThreadIndex, { drawable, sourceBatchIndex, unlitBasePass, nullptr });
            litBatches_.Insert(workerThreadIndex, { drawable, sourceBatchIndex, nullptr, lightPass });
            return true;
        }
        else
        {
            assert(0);
            return false;
        }
    }
    else
    {
        unlitBatches_.Insert(workerThreadIndex, { drawable, sourceBatchIndex, unlitBasePass, nullptr });
        return false;
    }
}

void ScenePass::CollectSceneBatches(unsigned mainLightIndex, ea::span<SceneLight*> sceneLights,
    const DrawableLightingData& drawableLighting, Camera* camera, ScenePipelineStateCacheCallback& callback)
{
    CollectUnlitBatches(camera, callback);
    CollectLitBatches(camera, callback, mainLightIndex, sceneLights, drawableLighting);
}

void ScenePass::CollectUnlitBatches(Camera* camera, ScenePipelineStateCacheCallback& callback)
{
    unlitBaseBatches_.resize(unlitBatches_.Size());
    ForEachParallel(workQueue_, BatchThreshold, unlitBatches_,
        [&](unsigned threadIndex, unsigned offset, ea::span<IntermediateSceneBatch const> batches)
    {
        Material* defaultMaterial = renderer_->GetDefaultMaterial();
        for (unsigned i = 0; i < batches.size(); ++i)
        {
            const IntermediateSceneBatch& intermediateBatch = batches[i];
            BaseSceneBatch& sceneBatch = unlitBaseBatches_[i + offset];

            // Add base batch
            sceneBatch = BaseSceneBatch{ M_MAX_UNSIGNED, intermediateBatch, defaultMaterial };
            sceneBatch.pipelineState_ = unlitPipelineStateCache_.GetPipelineState({ sceneBatch, 0 });
            if (!sceneBatch.pipelineState_)
                unlitBaseBatchesDirty_.Insert(threadIndex, &sceneBatch);
        }
    });

    ScenePipelineStateContext subPassContext;
    subPassContext.camera_ = camera;
    subPassContext.light_ = nullptr;

    unlitBaseBatchesDirty_.ForEach([&](unsigned, unsigned, BaseSceneBatch* sceneBatch)
    {
        const ScenePipelineStateKey key{ *sceneBatch, 0 };
        subPassContext.drawable_ = sceneBatch->drawable_;
        sceneBatch->pipelineState_ = unlitPipelineStateCache_.GetOrCreatePipelineState(key, subPassContext, callback);
    });
}

void ScenePass::CollectLitBatches(Camera* camera, ScenePipelineStateCacheCallback& callback,
    unsigned mainLightIndex, ea::span<SceneLight*> sceneLights, const DrawableLightingData& drawableLighting)
{
    litBaseBatches_.resize(litBatches_.Size());

    const unsigned mainLightHash = mainLightIndex != M_MAX_UNSIGNED
        ? sceneLights[mainLightIndex]->GetPipelineStateHash()
        : 0;

    ForEachParallel(workQueue_, BatchThreshold, litBatches_,
        [&](unsigned threadIndex, unsigned offset, ea::span<IntermediateSceneBatch const> batches)
    {
        Material* defaultMaterial = renderer_->GetDefaultMaterial();
        for (unsigned i = 0; i < batches.size(); ++i)
        {
            const IntermediateSceneBatch& intermediateBatch = batches[i];
            BaseSceneBatch& sceneBatch = litBaseBatches_[i + offset];

            const auto pixelLights = drawableLighting[sceneBatch.drawableIndex_].GetPixelLights();
            const bool hasLitBase = !pixelLights.empty() && pixelLights[0].second == mainLightIndex;
            const unsigned baseLightIndex = hasLitBase ? mainLightIndex : M_MAX_UNSIGNED;
            const unsigned baseLightHash = hasLitBase ? mainLightHash : 0;

            // Add base batch
            sceneBatch = BaseSceneBatch{ baseLightIndex, intermediateBatch, defaultMaterial };
            sceneBatch.pipelineState_ = litPipelineStateCache_.GetPipelineState({ sceneBatch, baseLightHash });
            if (!sceneBatch.pipelineState_)
                litBaseBatchesDirty_.Insert(threadIndex, &sceneBatch);

            for (unsigned j = hasLitBase ? 1 : 0; j < pixelLights.size(); ++j)
            {
                const unsigned lightIndex = pixelLights[j].second;
                const unsigned lightHash = sceneLights[lightIndex]->GetPipelineStateHash();

                BaseSceneBatch lightBatch = sceneBatch;
                lightBatch.lightIndex_ = lightIndex;
                lightBatch.pass_ = intermediateBatch.additionalPass_;

                lightBatch.pipelineState_ = additionalLightPipelineStateCache_.GetPipelineState({ lightBatch, lightHash });
                const unsigned batchIndex = lightBatches_.Insert(threadIndex, lightBatch);
                if (!lightBatch.pipelineState_)
                    lightBatchesDirty_.Insert(threadIndex, batchIndex);
            }
        }
    });

    // Resolve base pipeline states
    {
        ScenePipelineStateContext baseSubPassContext;
        baseSubPassContext.camera_ = camera;
        baseSubPassContext.light_ = mainLightIndex != M_MAX_UNSIGNED ? sceneLights[mainLightIndex] : nullptr;
        const unsigned baseLightHash = mainLightIndex != M_MAX_UNSIGNED ? mainLightHash : 0;

        litBaseBatchesDirty_.ForEach([&](unsigned, unsigned, BaseSceneBatch* sceneBatch)
        {
            baseSubPassContext.drawable_ = sceneBatch->drawable_;
            const ScenePipelineStateKey baseKey{ *sceneBatch, baseLightHash };
            sceneBatch->pipelineState_ = litPipelineStateCache_.GetOrCreatePipelineState(
                baseKey, baseSubPassContext, callback);
        });
    }

    // Resolve light pipeline states
    {
        ScenePipelineStateContext lightSubPassContext;
        lightSubPassContext.camera_ = camera;

        lightBatchesDirty_.ForEach([&](unsigned threadIndex, unsigned, unsigned batchIndex)
        {
            BaseSceneBatch& lightBatch = lightBatches_.Get(threadIndex, batchIndex);
            const SceneLight* sceneLight = sceneLights[lightBatch.lightIndex_];
            lightSubPassContext.light_ = sceneLight;
            lightSubPassContext.drawable_ = lightBatch.drawable_;

            const ScenePipelineStateKey lightKey{ lightBatch, sceneLight->GetPipelineStateHash() };
            lightBatch.pipelineState_ = additionalLightPipelineStateCache_.GetOrCreatePipelineState(
                lightKey, lightSubPassContext, callback);
        });
    }
}

ForwardLightingScenePass::ForwardLightingScenePass(Context* context,
    const ea::string& unlitBasePassIndex, const ea::string& litBasePassIndex, const ea::string& lightPassIndex)
    : ScenePass(context,
        Technique::GetPassIndex(unlitBasePassIndex),
        Technique::GetPassIndex(litBasePassIndex),
        Technique::GetPassIndex(lightPassIndex))
{
}

#if 0
OpaqueForwardLightingScenePass::OpaqueForwardLightingScenePass(Context* context,
    const ea::string& unlitBasePassIndex, const ea::string& litBasePassIndex, const ea::string& lightPassIndex)
    : ForwardLightingScenePass(context, unlitBasePassIndex, litBasePassIndex, lightPassIndex)
{
}
#endif

bool OpaqueForwardLightingScenePass::IsValid() const
{
    return unlitBasePassIndex_ != M_MAX_UNSIGNED
        && litBasePassIndex_ != M_MAX_UNSIGNED
        && lightPassIndex_ != M_MAX_UNSIGNED;
}

void OpaqueForwardLightingScenePass::SortSceneBatches()
{
    SortBatches(unlitBaseBatches_, sortedUnlitBaseBatches_);
    SortBatches(litBaseBatches_, sortedLitBaseBatches_);
    SortBatches(lightBatches_, sortedLightBatches_);
}

}