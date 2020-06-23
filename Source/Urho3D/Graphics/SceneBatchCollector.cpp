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
#include "../Core/IteratorRange.h"
#include "../Core/WorkQueue.h"
#include "../Graphics/Camera.h"
#include "../Graphics/Material.h"
#include "../Graphics/Octree.h"
#include "../Graphics/OctreeQuery.h"
#include "../Graphics/Renderer.h"
#include "../Graphics/SceneBatchCollector.h"
#include "../Scene/Scene.h"

#include "../DebugNew.h"

namespace Urho3D
{

namespace
{

/// Frustum Query for point light.
struct PointLightLitGeometriesQuery : public SphereOctreeQuery
{
    /// Return light sphere for the query.
    static Sphere GetLightSphere(Light* light)
    {
        return Sphere(light->GetNode()->GetWorldPosition(), light->GetRange());
    }

    /// Construct.
    PointLightLitGeometriesQuery(ea::vector<Drawable*>& result,
        const TransientDrawableIndex& transientData, Light* light)
        : SphereOctreeQuery(result, GetLightSphere(light), DRAWABLE_GEOMETRY)
        , transientData_(&transientData)
        , lightMask_(light->GetLightMaskEffective())
    {
    }

    void TestDrawables(Drawable** start, Drawable** end, bool inside) override
    {
        for (Drawable* drawable : MakeIteratorRange(start, end))
        {
            const unsigned drawableIndex = drawable->GetDrawableIndex();
            const unsigned traits = transientData_->traits_[drawableIndex];
            if (traits & TransientDrawableIndex::DrawableVisibleGeometry)
            {
                if (drawable->GetLightMask() & lightMask_)
                {
                    if (inside || sphere_.IsInsideFast(drawable->GetWorldBoundingBox()))
                        result_.push_back(drawable);
                }
            }
        }
    }

    /// Visiblity cache.
    const TransientDrawableIndex* transientData_{};
    /// Light mask to check.
    unsigned lightMask_{};
};

/// Frustum Query for spot light.
struct SpotLightLitGeometriesQuery : public FrustumOctreeQuery
{
    /// Construct.
    SpotLightLitGeometriesQuery(ea::vector<Drawable*>& result,
        const TransientDrawableIndex& transientData, Light* light)
        : FrustumOctreeQuery(result, light->GetFrustum(), DRAWABLE_GEOMETRY)
        , transientData_(&transientData)
        , lightMask_(light->GetLightMaskEffective())
    {
    }

    void TestDrawables(Drawable** start, Drawable** end, bool inside) override
    {
        for (Drawable* drawable : MakeIteratorRange(start, end))
        {
            const unsigned drawableIndex = drawable->GetDrawableIndex();
            const unsigned traits = transientData_->traits_[drawableIndex];
            if (traits & TransientDrawableIndex::DrawableVisibleGeometry)
            {
                if (drawable->GetLightMask() & lightMask_)
                {
                    if (inside || frustum_.IsInsideFast(drawable->GetWorldBoundingBox()))
                        result_.push_back(drawable);
                }
            }
        }
    }

    /// Visiblity cache.
    const TransientDrawableIndex* transientData_{};
    /// Light mask to check.
    unsigned lightMask_{};
};
}

struct SceneBatchCollector::IntermediateSceneBatch
{
    /// Geometry.
    Drawable* geometry_{};
    /// Index of source batch within geometry.
    unsigned sourceBatchIndex_{};
    /// Base material pass.
    Pass* basePass_{};
    /// Additional material pass for forward rendering.
    Pass* additionalPass_{};
};

struct SceneBatchCollector::PassData
{
    /// Pass description.
    ScenePassDescription desc_;
    /// Base pass index.
    unsigned basePassIndex_{};
    /// First light pass index.
    unsigned firstLightPassIndex_{};
    /// Additional light pass index.
    unsigned additionalLightPassIndex_{};

    /// Unlit intermediate batches.
    ThreadedVector<IntermediateSceneBatch> unlitBatches_;
    /// Lit intermediate batches. Always empty for Unlit passes.
    ThreadedVector<IntermediateSceneBatch> litBatches_;

    /// Unlit base scene batches.
    ea::vector<SceneBatch> unlitBaseSceneBatches_;
    /// Lit base scene batches.
    ea::vector<SceneBatch> litBaseSceneBatches_;

    /// Return whether given subpasses are present.
    bool CheckSubPasses(bool hasBase, bool hasFirstLight, bool hasAdditionalLight) const
    {
        return (basePassIndex_ != M_MAX_UNSIGNED) == hasBase
            && (firstLightPassIndex_ != M_MAX_UNSIGNED) == hasFirstLight
            && (additionalLightPassIndex_ != M_MAX_UNSIGNED) == hasAdditionalLight;
    }

    /// Return whether is valid.
    bool IsValid() const
    {
        switch (desc_.type_)
        {
        case ScenePassType::Unlit:
            return CheckSubPasses(true, false, false);
        case ScenePassType::ForwardLitBase:
            return CheckSubPasses(false, true, true) || CheckSubPasses(true, true, true);
        case ScenePassType::ForwardUnlitBase:
            return CheckSubPasses(true, false, true);
        default:
            return false;
        }
    }

    /// Create intermediate scene batch. Batch is not added to any queue.
    IntermediateSceneBatch CreateIntermediateSceneBatch(Drawable* geometry, unsigned sourceBatchIndex,
        Pass* basePass, Pass* firstLightPass, Pass* additionalLightPass) const
    {
        if (desc_.type_ == ScenePassType::Unlit || !additionalLightPass)
            return { geometry, sourceBatchIndex, basePass, nullptr };
        else if (desc_.type_ == ScenePassType::ForwardUnlitBase && basePass && additionalLightPass)
            return { geometry, sourceBatchIndex, basePass, additionalLightPass };
        else if (desc_.type_ == ScenePassType::ForwardLitBase && firstLightPass && additionalLightPass)
            return { geometry, sourceBatchIndex, firstLightPass, additionalLightPass };
        else
            return {};
    }

    /// Clear state before rendering.
    void Clear(unsigned numThreads)
    {
        unlitBatches_.Clear(numThreads);
        litBatches_.Clear(numThreads);
    }
};

struct SceneBatchCollector::DrawableZRangeEvaluator
{
    explicit DrawableZRangeEvaluator(Camera* camera)
        : viewMatrix_(camera->GetView())
        , viewZ_(viewMatrix_.m20_, viewMatrix_.m21_, viewMatrix_.m22_)
        , absViewZ_(viewZ_.Abs())
    {
    }

    DrawableZRange Evaluate(Drawable* drawable) const
    {
        const BoundingBox& boundingBox = drawable->GetWorldBoundingBox();
        const Vector3 center = boundingBox.Center();
        const Vector3 edge = boundingBox.Size() * 0.5f;

        // Ignore "infinite" objects like skybox
        if (edge.LengthSquared() >= M_LARGE_VALUE * M_LARGE_VALUE)
            return {};

        const float viewCenterZ = viewZ_.DotProduct(center) + viewMatrix_.m23_;
        const float viewEdgeZ = absViewZ_.DotProduct(edge);
        const float minZ = viewCenterZ - viewEdgeZ;
        const float maxZ = viewCenterZ + viewEdgeZ;

        return { minZ, maxZ };
    }

    Matrix3x4 viewMatrix_;
    Vector3 viewZ_;
    Vector3 absViewZ_;
};

struct SceneBatchCollector::LightData
{
    /// Lit geometries.
    // TODO: Ignore unlit geometries?
    ea::vector<Drawable*> litGeometries_;

    /// Clear.
    void Clear()
    {
        litGeometries_.clear();
    }
};

SceneBatchCollector::SceneBatchCollector(Context* context)
    : Object(context)
    , workQueue_(context->GetWorkQueue())
    , renderer_(context->GetRenderer())
{}

SceneBatchCollector::~SceneBatchCollector()
{
}

Technique* SceneBatchCollector::FindTechnique(Drawable* drawable, Material* material) const
{
    const ea::vector<TechniqueEntry>& techniques = material->GetTechniques();

    // If only one technique, no choice
    if (techniques.size() == 1)
        return techniques[0].technique_;

    // TODO: Consider optimizing this loop
    const float lodDistance = drawable->GetLodDistance();
    for (unsigned i = 0; i < techniques.size(); ++i)
    {
        const TechniqueEntry& entry = techniques[i];
        Technique* tech = entry.technique_;

        if (!tech || (!tech->IsSupported()) || materialQuality_ < entry.qualityLevel_)
            continue;
        if (lodDistance >= entry.lodDistance_)
            return tech;
    }

    // If no suitable technique found, fallback to the last
    return techniques.size() ? techniques.back().technique_ : nullptr;
}

void SceneBatchCollector::InitializeFrame(const FrameInfo& frameInfo)
{
    numThreads_ = workQueue_->GetNumThreads() + 1;
    materialQuality_ = renderer_->GetMaterialQuality();

    frameInfo_ = frameInfo;
    octree_ = frameInfo.octree_;
    camera_ = frameInfo.camera_;
    numDrawables_ = octree_->GetAllDrawables().size();

    if (camera_->GetViewOverrideFlags() & VO_LOW_MATERIAL_QUALITY)
        materialQuality_ = QUALITY_LOW;

    visibleGeometries_.Clear(numThreads_);
    visibleLightsTemp_.Clear(numThreads_);
    sceneZRange_.Clear(numThreads_);

    transient_.Reset(numDrawables_);
    drawableLighting_.resize(numDrawables_);
}

void SceneBatchCollector::InitializePasses(ea::span<const ScenePassDescription> passes)
{
    const unsigned numPasses = passes.size();
    passes_.resize(numPasses);
    for (unsigned i = 0; i < numPasses; ++i)
    {
        PassData& passData = passes_[i];
        passData.desc_ = passes[i];

        passData.basePassIndex_ = Technique::GetPassIndex(passData.desc_.basePassName_);
        passData.firstLightPassIndex_ = Technique::GetPassIndex(passData.desc_.firstLightPassName_);
        passData.additionalLightPassIndex_ = Technique::GetPassIndex(passData.desc_.additionalLightPassName_);

        if (!passData.IsValid())
        {
            // TODO: Log error
            assert(0);
            continue;
        }

        passData.Clear(numThreads_);
    }
}

void SceneBatchCollector::UpdateAndCollectSourceBatches(const ea::vector<Drawable*>& drawables)
{
    ForEachParallel(workQueue_, drawableWorkThreshold_, drawables,
        [this](unsigned threadIndex, unsigned /*offset*/, ea::span<Drawable* const> drawablesRange)
    {
        UpdateAndCollectSourceBatchesForThread(threadIndex, drawablesRange);
    });

    // Copy results from intermediate collection
    visibleLightsTemp_.CopyTo(visibleLights_);
}

void SceneBatchCollector::UpdateAndCollectSourceBatchesForThread(unsigned threadIndex, ea::span<Drawable* const> drawables)
{
    Material* defaultMaterial = renderer_->GetDefaultMaterial();
    const DrawableZRangeEvaluator zRangeEvaluator{ camera_ };

    for (Drawable* drawable : drawables)
    {
        // TODO: Add occlusion culling
        const unsigned drawableIndex = drawable->GetDrawableIndex();

        drawable->UpdateBatches(frameInfo_);
        transient_.traits_[drawableIndex] |= TransientDrawableIndex::DrawableUpdated;

        // Skip if too far
        const float maxDistance = drawable->GetDrawDistance();
        if (maxDistance > 0.0f)
        {
            if (drawable->GetDistance() > maxDistance)
                return;
        }

        // For geometries, find zone, clear lights and calculate view space Z range
        if (drawable->GetDrawableFlags() & DRAWABLE_GEOMETRY)
        {
            const DrawableZRange zRange = zRangeEvaluator.Evaluate(drawable);

            // Do not add "infinite" objects like skybox to prevent shadow map focusing behaving erroneously
            if (!zRange.IsValid())
                transient_.zRange_[drawableIndex] = { M_LARGE_VALUE, M_LARGE_VALUE };
            else
            {
                transient_.zRange_[drawableIndex] = zRange;
                sceneZRange_.Accumulate(threadIndex, zRange);
            }

            visibleGeometries_.Insert(threadIndex, drawable);
            transient_.traits_[drawableIndex] |= TransientDrawableIndex::DrawableVisibleGeometry;

            // Collect batches
            const auto& sourceBatches = drawable->GetBatches();
            for (unsigned i = 0; i < sourceBatches.size(); ++i)
            {
                const SourceBatch& sourceBatch = sourceBatches[i];

                // Find current technique
                Material* material = sourceBatch.material_ ? sourceBatch.material_ : defaultMaterial;
                Technique* technique = FindTechnique(drawable, material);
                if (!technique)
                    continue;

                // Fill passes
                for (PassData& pass : passes_)
                {
                    Pass* basePass = technique->GetPass(pass.basePassIndex_);
                    Pass* firstLightPass = technique->GetPass(pass.firstLightPassIndex_);
                    Pass* additionalLightPass = technique->GetPass(pass.additionalLightPassIndex_);

                    const IntermediateSceneBatch sceneBatch = pass.CreateIntermediateSceneBatch(
                        drawable, i, basePass, firstLightPass, additionalLightPass);

                    if (sceneBatch.additionalPass_)
                    {
                        transient_.traits_[drawableIndex] |= TransientDrawableIndex::ForwardLit;
                        pass.litBatches_.Insert(threadIndex, sceneBatch);
                    }
                    else if (sceneBatch.basePass_)
                        pass.unlitBatches_.Insert(threadIndex, sceneBatch);
                }
            }

            // Reset light accumulator
            // TODO: Don't do it if unlit
            drawableLighting_[drawableIndex].Reset();
        }
        else if (drawable->GetDrawableFlags() & DRAWABLE_LIGHT)
        {
            auto light = static_cast<Light*>(drawable);
            const Color lightColor = light->GetEffectiveColor();

            // Skip lights with zero brightness or black color, skip baked lights too
            if (!lightColor.Equals(Color::BLACK) && light->GetLightMaskEffective() != 0)
                visibleLightsTemp_.Insert(threadIndex, light);
        }
    }
}

void SceneBatchCollector::ProcessVisibleLights()
{
    // Allocate internal storage for lights
    visibleLightsData_.clear();
    for (Light* light : visibleLights_)
    {
        WeakPtr<Light> weakLight(light);
        auto& lightData = cachedLightData_[weakLight];
        if (!lightData)
            lightData = ea::make_unique<LightData>();

        lightData->Clear();
        visibleLightsData_.push_back(lightData.get());
    };

    // Process lights in worker threads
    for (unsigned i = 0; i < visibleLights_.size(); ++i)
    {
        workQueue_->AddWorkItem([this, i](unsigned threadIndex)
        {
            Light* light = visibleLights_[i];
            LightData& lightData = *visibleLightsData_[i];
            ProcessLightThreaded(light, lightData);
        });
    }
    workQueue_->Complete(M_MAX_UNSIGNED);

    // Accumulate lighting
    for (unsigned i = 0; i < visibleLights_.size(); ++i)
        AccumulateForwardLighting(i);
}

void SceneBatchCollector::ProcessLightThreaded(Light* light, LightData& lightData)
{
    CollectLitGeometries(light, lightData);
}

void SceneBatchCollector::CollectLitGeometries(Light* light, LightData& lightData)
{
    switch (light->GetLightType())
    {
    case LIGHT_SPOT:
    {
        SpotLightLitGeometriesQuery query(lightData.litGeometries_, transient_, light);
        octree_->GetDrawables(query);
        break;
    }
    case LIGHT_POINT:
    {
        PointLightLitGeometriesQuery query(lightData.litGeometries_, transient_, light);
        octree_->GetDrawables(query);
        break;
    }
    case LIGHT_DIRECTIONAL:
    {
        const unsigned lightMask = light->GetLightMask();
        visibleGeometries_.ForEach([&](unsigned index, Drawable* drawable)
        {
            if (drawable->GetLightMask() & lightMask)
                lightData.litGeometries_.push_back(drawable);
        });
        break;
    }
    }
}

void SceneBatchCollector::AccumulateForwardLighting(unsigned lightIndex)
{
    Light* light = visibleLights_[lightIndex];
    LightData& lightData = *visibleLightsData_[lightIndex];

    ForEachParallel(workQueue_, litGeometriesWorkThreshold_, lightData.litGeometries_,
        [&](unsigned /*threadIndex*/, unsigned /*offset*/, ea::span<Drawable* const> geometries)
    {
        DrawableLightDataAccumulationContext accumContext;
        accumContext.maxPixelLights_ = 1;
        accumContext.lightImportance_ = light->GetLightImportance();
        accumContext.lightIndex_ = lightIndex;
        accumContext.lights_ = &visibleLights_;

        const float lightIntensityPenalty = 1.0f / light->GetIntensityDivisor();

        for (Drawable* geometry : geometries)
        {
            const unsigned drawableIndex = geometry->GetDrawableIndex();
            const float distance = light->GetDistanceTo(geometry);
            drawableLighting_[drawableIndex].AccumulateLight(accumContext, distance * lightIntensityPenalty);
        }
    });
}

void SceneBatchCollector::CollectSceneBatches()
{
    for (PassData& passData : passes_)
    {
        passData.unlitBaseSceneBatches_.resize(passData.unlitBatches_.Size());
        passData.litBaseSceneBatches_.resize(passData.litBatches_.Size());

        ForEachParallel(workQueue_, batchWorkThreshold_, passData.unlitBatches_,
            [&](unsigned /*threadIndex*/, unsigned offset, ea::span<IntermediateSceneBatch const> batches)
        {
            Material* defaultMaterial = renderer_->GetDefaultMaterial();
            for (unsigned i = 0; i < batches.size(); ++i)
            {
                const IntermediateSceneBatch& intermediateBatch = batches[i];
                SceneBatch& sceneBatch = passData.unlitBaseSceneBatches_[i + offset];

                Drawable* drawable = intermediateBatch.geometry_;
                const SourceBatch& sourceBatch = drawable->GetBatches()[intermediateBatch.sourceBatchIndex_];

                sceneBatch.drawable_ = drawable;
                sceneBatch.drawableIndex_ = drawable->GetDrawableIndex();
                sceneBatch.sourceBatchIndex_ = intermediateBatch.sourceBatchIndex_;
                sceneBatch.geometry_ = sourceBatch.geometry_;
                sceneBatch.material_ = sourceBatch.material_ ? sourceBatch.material_ : defaultMaterial;
            }
        });
    }
}

}
