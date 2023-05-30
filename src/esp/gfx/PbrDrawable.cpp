// Copyright (c) Meta Platforms, Inc. and its affiliates.
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "PbrDrawable.h"

#include <Corrade/Containers/ArrayViewStl.h>
#include <Corrade/Utility/FormatStl.h>
#include <Magnum/Trade/MaterialData.h>
#include <Magnum/Trade/PbrClearCoatMaterialData.h>
#include <Magnum/Trade/PbrMetallicRoughnessMaterialData.h>

#include <Magnum/GL/Renderer.h>

using Magnum::Math::Literals::operator""_radf;
namespace Mn = Magnum;

namespace esp {
namespace gfx {

PbrDrawable::PbrDrawable(scene::SceneNode& node,
                         Mn::GL::Mesh* mesh,
                         gfx::Drawable::Flags& meshAttributeFlags,
                         ShaderManager& shaderManager,
                         const Mn::ResourceKey& lightSetupKey,
                         const Mn::ResourceKey& materialDataKey,
                         DrawableGroup* group,
                         PbrImageBasedLighting* pbrIbl)
    : Drawable{node, mesh, DrawableType::Pbr, group},
      shaderManager_{shaderManager},
      lightSetup_{shaderManager.get<LightSetup>(lightSetupKey)},
      meshAttributeFlags_{meshAttributeFlags},
      pbrIbl_(pbrIbl) {
  setMaterialValuesInternal(
      shaderManager.get<Mn::Trade::MaterialData>(materialDataKey));

  if (pbrIbl_) {
    flags_ |= PbrShader::Flag::ImageBasedLighting;
  }

  // Defer the shader initialization because at this point, the lightSetup may
  // not be done in the Simulator. Simulator itself is currently under
  // construction in this case.
  // updateShader().updateShaderLightParameters();
}

void PbrDrawable::setMaterialValuesInternal(
    const Mn::Resource<Mn::Trade::MaterialData, Mn::Trade::MaterialData>&
        material) {
  materialData_ = material;

  const auto& tmpMaterialData =
      materialData_->as<Mn::Trade::PbrMetallicRoughnessMaterialData>();
  flags_ = PbrShader::Flag::ObjectId;

  matCache.baseColor = tmpMaterialData.baseColor();
  matCache.roughness = tmpMaterialData.roughness();
  matCache.metalness = tmpMaterialData.metalness();
  matCache.emissiveColor = tmpMaterialData.emissiveColor();

  if (tmpMaterialData.commonTextureMatrix() != Mn::Matrix3{}) {
    flags_ |= PbrShader::Flag::TextureTransformation;
    matCache.textureMatrix = tmpMaterialData.commonTextureMatrix();
  }
  if (const auto baseColorTexturePtr =
          materialData_->findAttribute<Mn::GL::Texture2D*>(
              "baseColorTexturePointer")) {
    flags_ |= PbrShader::Flag::BaseColorTexture;
    matCache.baseColorTexture = *baseColorTexturePtr;
  }

  if (const auto noneRoughMetalTexturePtr =
          materialData_->findAttribute<Mn::GL::Texture2D*>(
              "noneRoughnessMetallicTexturePointer")) {
    flags_ |= PbrShader::Flag::NoneRoughnessMetallicTexture;
    matCache.noneRoughnessMetallicTexture = *noneRoughMetalTexturePtr;
  }

  if (const auto normalTexturePtr =
          materialData_->findAttribute<Mn::GL::Texture2D*>(
              "normalTexturePointer")) {
    flags_ |= PbrShader::Flag::NormalTexture;
    matCache.normalTexture = *normalTexturePtr;
    if (meshAttributeFlags_ & gfx::Drawable::Flag::HasTangent) {
      flags_ |= PbrShader::Flag::PrecomputedTangent;
    }
    // normal texture scale
    matCache.normalTextureScale = tmpMaterialData.normalTextureScale();
  }

  if (const auto emissiveTexturePtr =
          materialData_->findAttribute<Mn::GL::Texture2D*>(
              "emissiveTexturePointer")) {
    flags_ |= PbrShader::Flag::EmissiveTexture;
    matCache.emissiveTexture = *emissiveTexturePtr;
  }
  if (materialData_->attribute<bool>("hasPerVertexObjectId")) {
    flags_ |= PbrShader::Flag::InstancedObjectId;
  }
  if (materialData_->isDoubleSided()) {
    flags_ |= PbrShader::Flag::DoubleSided;
  }

  ////////////////
  // ClearCoat layer
  if (materialData_->hasLayer(Mn::Trade::MaterialLayer::ClearCoat)) {
    const auto& ccLayer =
        materialData_->as<Mn::Trade::PbrClearCoatMaterialData>();
    float cc_LayerFactor = ccLayer.layerFactor();
    // As per
    // https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_clearcoat
    // if layer is 0 entire layer is disabled/ignored.
    if (cc_LayerFactor > 0.0f) {
      // has non-trivial clearcoat layer
      flags_ |= PbrShader::Flag::ClearCoatLayer;
      //
      matCache.clearCoat.factor = cc_LayerFactor;
      matCache.clearCoat.roughnessFactor = ccLayer.roughness();
      if (const auto layerTexturePtr =
              ccLayer.findAttribute<Mn::GL::Texture2D*>(
                  "layerFactorTexturePointer")) {
        flags_ |= PbrShader::Flag::ClearCoatTexture;
        matCache.clearCoat.texture = *layerTexturePtr;
      }

      if (const auto roughnessTexturePtr =
              ccLayer.findAttribute<Mn::GL::Texture2D*>(
                  "roughnessTexturePointer")) {
        flags_ |= PbrShader::Flag::ClearCoatRoughnessTexture;
        matCache.clearCoat.roughnessTexture = *roughnessTexturePtr;
      }

      if (const auto normalTexturePtr =
              ccLayer.findAttribute<Mn::GL::Texture2D*>(
                  "normalTexturePointer")) {
        flags_ |= PbrShader::Flag::ClearCoatNormalTexture;
        matCache.clearCoat.normalTexture = *normalTexturePtr;
        matCache.clearCoat.normalTextureScale = ccLayer.normalTextureScale();
      }

    }  // non-zero layer factor
  }    // has clearcoat layer

  ////////////////
  // KHR_materials_ior
  if (const auto iorLayerID =
          materialData_->findLayerId("#KHR_materials_ior")) {
    // Read in custom material index of refraction
    if (const auto ior =
            materialData_->findAttribute<Mn::Float>(*iorLayerID, "ior")) {
      // ior should be >= 1 or 0 (which gives full weight to specular layer
      // independent of view angle)
      matCache.ior_Index = *ior;
    }

  }  // has KHR_materials_ior layer

  ////////////////
  // KHR_materials_specular layer
  if (const auto specularLayerID =
          materialData_->findLayerId("#KHR_materials_specular")) {
    flags_ |= PbrShader::Flag::SpecularLayer;
    /**
     * The strength of the specular reflection. Defaults to 1.0f
     */
    if (const auto specularFactor = materialData_->findAttribute<Mn::Float>(
            *specularLayerID, "specularFactor")) {
      matCache.specularLayer.factor =
          Mn::Math::clamp(*specularFactor, 0.0f, 1.0f);
    }

    /**
     * A texture that defines the strength of the specular
     * reflection, stored in the alpha (A) channel. This will be
     * multiplied by specularFactor.
     */
    if (const auto specularFactorTexture =
            materialData_->findAttribute<Mn::GL::Texture2D*>(
                *specularLayerID, "specularTexturePointer")) {
      flags_ |= PbrShader::Flag::SpecularLayerTexture;
      matCache.specularLayer.texture = *specularFactorTexture;
    }
    /**
     * The F0 color of the specular reflection (linear RGB).
     */
    if (const auto specularColorFactor =
            materialData_->findAttribute<Mn::Color3>(*specularLayerID,
                                                     "specularColorFactor")) {
      matCache.specularLayer.colorFactor = *specularColorFactor;
    }
    /**
     * A texture that defines the F0 color of the specular
     * reflection, stored in the RGB channels and encoded in
     * sRGB. This texture will be multiplied by
     * specularColorFactor.
     */
    if (const auto specularColorTexture =
            materialData_->findAttribute<Mn::GL::Texture2D*>(
                *specularLayerID, "specularColorTexturePointer")) {
      flags_ |= PbrShader::Flag::SpecularLayerColorTexture;
      matCache.specularLayer.colorTexture = *specularColorTexture;
    }
  }  // has KHR_materials_specular layer

  ///////////////
  // KHR_materials_anisotropy
  if (const auto anisotropyLayerID =
          materialData_->findLayerId("#KHR_materials_anisotropy")) {
    /**
     * The anisotropy strength. When anisotropyTexture is present, this value is
     * multiplied by the blue channel. Default is 0.0f
     */
    if (const auto anisotropyStrength = materialData_->findAttribute<Mn::Float>(
            *anisotropyLayerID, "anisotropyStrength")) {
      if (Mn::Math::abs(*anisotropyStrength) > 0.0) {
        flags_ |= PbrShader::Flag::AnisotropyLayer;
        matCache.anisotropyLayer.factor =
            Mn::Math::clamp(*anisotropyStrength, -1.0f, 1.0f);
      }
      // Early adopters used anisotropy to mean strength
    } else if (const auto anisotropyStrength =
                   materialData_->findAttribute<Mn::Float>(*anisotropyLayerID,
                                                           "anisotropy")) {
      if (Mn::Math::abs(*anisotropyStrength) > 0.0) {
        flags_ |= PbrShader::Flag::AnisotropyLayer;
        matCache.anisotropyLayer.factor =
            Mn::Math::clamp(*anisotropyStrength, -1.0f, 1.0f);
      }
    }
    /**
     * The rotation of the anisotropy in tangent, bitangent space, measured in
     * radians counter-clockwise from the tangent. When anisotropyTexture is
     * present, anisotropyRotation provides additional rotation to the vectors
     * in the texture. Default is 0.0f
     */
    if (const auto anisotropyRotation = materialData_->findAttribute<Mn::Float>(
            *anisotropyLayerID, "anisotropyRotation")) {
      if (*anisotropyRotation != 0.0) {
        flags_ |= PbrShader::Flag::AnisotropyLayer;
        Mn::Rad rotAngle = Mn::Rad{*anisotropyRotation};
        matCache.anisotropyLayer.direction =
            Mn::Vector2{Mn::Complex::rotation(rotAngle)};
      }
      // Early adopters used anisotropyDirection
    } else if (const auto anisotropyRotation =
                   materialData_->findAttribute<Mn::Float>(
                       *anisotropyLayerID, "anisotropyDirection")) {
      if (*anisotropyRotation != 0.0) {
        flags_ |= PbrShader::Flag::AnisotropyLayer;
        Mn::Rad rotAngle = Mn::Rad{*anisotropyRotation};
        matCache.anisotropyLayer.direction =
            Mn::Vector2{Mn::Complex::rotation(rotAngle)};
      }
    }

    /**
     * A texture that defines the anisotropy of the material. Red and green
     * channels represent the anisotropy direction in [-1, 1] tangent,
     * bitangent space, to be rotated by anisotropyRotation. The blue
     * channel contains strength as [0, 1] to be multiplied by
     * anisotropyStrength.
     */
    if (const auto anisotropyLayerTexture =
            materialData_->findAttribute<Mn::GL::Texture2D*>(
                *anisotropyLayerID, "anisotropyTexturePointer")) {
      // also covers flags_ |= PbrShader::Flag::AnisotropyLayer;
      flags_ |= PbrShader::Flag::AnisotropyLayerTexture;
      matCache.anisotropyLayer.texture = *anisotropyLayerTexture;
    }
  }  // has KHR_materials_anisotropy

  ////////////////
  // KHR_materials_transmission
  if (const auto transmissionLayerID =
          materialData_->findLayerId("#KHR_materials_transmission")) {
    flags_ |= PbrShader::Flag::TransmissionLayer;
    // transmissionFactor
    if (const auto transmissionFactor = materialData_->findAttribute<Mn::Float>(
            *transmissionLayerID, "transmissionFactor")) {
      matCache.transmissionLayer.factor = *transmissionFactor;
    }
    // transmissionTexturePointer
    if (const auto transmissionTexturePointer =
            materialData_->findAttribute<Mn::GL::Texture2D*>(
                *transmissionLayerID, "transmissionTexturePointer")) {
      flags_ |= PbrShader::Flag::TransmissionLayerTexture;
      matCache.transmissionLayer.texture = *transmissionTexturePointer;
    }
  }  // has KHR_materials_transmission layer

  ////////////////
  // KHR_materials_volume
  if (const auto volumeLayerID =
          materialData_->findLayerId("#KHR_materials_volume")) {
    flags_ |= PbrShader::Flag::VolumeLayer;

    if (const auto thicknessFactor = materialData_->findAttribute<Mn::Float>(
            *volumeLayerID, "thicknessFactor")) {
      matCache.volumeLayer.thicknessFactor = *thicknessFactor;
    }

    if (const auto thicknessTexturePointer =
            materialData_->findAttribute<Mn::GL::Texture2D*>(
                *volumeLayerID, "thicknessTexturePointer")) {
      flags_ |= PbrShader::Flag::VolumeLayerThicknessTexture;
      matCache.volumeLayer.thicknessTexture = *thicknessTexturePointer;
    }

    if (const auto attDist = materialData_->findAttribute<Mn::Float>(
            *volumeLayerID, "attenuationDistance")) {
      if (*attDist > 0.0f) {
        // Can't be 0 or inf
        matCache.volumeLayer.attenuationDist = *attDist;
      }
    }

    if (const auto attenuationColor = materialData_->findAttribute<Mn::Color3>(
            *volumeLayerID, "attenuationColor")) {
      matCache.volumeLayer.attenuationColor = *attenuationColor;
    }
  }  // has KHR_materials_volume layer
}  // PbrDrawable::setMaterialValuesInternal

void PbrDrawable::setLightSetup(const Mn::ResourceKey& lightSetupKey) {
  lightSetup_ = shaderManager_.get<LightSetup>(lightSetupKey);
}

void PbrDrawable::draw(const Mn::Matrix4& transformationMatrix,
                       Mn::SceneGraph::Camera3D& camera) {
  CORRADE_ASSERT(glMeshExists(),
                 "PbrDrawable::draw() : GL mesh doesn't exist", );

  updateShader()
      .updateShaderLightParameters()
      .updateShaderLightDirectionParameters(transformationMatrix, camera);

  // ABOUT PbrShader::Flag::DoubleSided:
  //
  // "Specifies whether the material is double sided. When this value is
  // false, back-face culling is enabled. When this value is true, back-face
  // culling is disabled and double sided lighting is enabled. The back-face
  // must have its normals reversed before the lighting equation is
  // evaluated." See here:
  // https://github.com/KhronosGroup/glTF/blob/master/specification/2.0/schema/material.schema.json

  // HOWEVER, WE CANNOT DISABLE BACK FACE CULLING (that is why the following
  // code is commented out) since it causes lighting artifacts ("dashed
  // lines") on hard edges. (maybe due to potential numerical issues? we do
  // not know yet.)
  /*
  if ((flags_ & PbrShader::Flag::DoubleSided) && glIsEnabled(GL_CULL_FACE))
  { Mn::GL::Renderer::disable(Mn::GL::Renderer::Feature::FaceCulling);
  }
  */
  Mn::Matrix4 modelMatrix =
      camera.cameraMatrix().inverted() * transformationMatrix;

  Mn::Matrix3x3 rotScale = modelMatrix.rotationScaling();
  // Find determinant to calculate backface culling winding dir
  const float normalDet = rotScale.determinant();
  // Normal matrix is calculated as `m.inverted().transposed()`, and
  // `m.inverted()` is the same as
  // `m.comatrix().transposed()/m.determinant()`. We need the determinant to
  // figure out the winding direction as well, thus we calculate it
  // separately and then do
  // `(m.comatrix().transposed()/determinant).transposed()`, which is the
  // same as `m.comatrix()/determinant`.
  Mn::Matrix3x3 normalMatrix = rotScale.comatrix() / normalDet;

  // Flip winding direction to correct handle backface culling
  if (normalDet < 0) {
    Mn::GL::Renderer::setFrontFace(Mn::GL::Renderer::FrontFace::ClockWise);
  }

  (*shader_)
      // e.g., semantic mesh has its own per vertex annotation, which has
      // been uploaded to GPU so simply pass 0 to the uniform "objectId" in
      // the fragment shader
      .setObjectId(static_cast<RenderCamera&>(camera).useDrawableIds()
                       ? drawableId_
                       : (flags_ >= PbrShader::Flag::InstancedObjectId
                              ? 0
                              : node_.getSemanticId()))
      .setProjectionMatrix(camera.projectionMatrix())
      .setViewMatrix(camera.cameraMatrix())
      .setModelMatrix(modelMatrix)  // NOT modelview matrix!
      .setNormalMatrix(normalMatrix)
      .setCameraWorldPosition(
          camera.object().absoluteTransformationMatrix().translation())
      .setBaseColor(matCache.baseColor)
      .setRoughness(matCache.roughness)
      .setMetallic(matCache.metalness)
      .setIndexOfRefraction(matCache.ior_Index)
      .setEmissiveColor(matCache.emissiveColor);

  // TODO:
  // IN PbrShader class, we set the resonable defaults for the
  // PbrShader::PbrEquationScales. Here we need a smart way to reset it
  // just in case user would like to do so during the run-time.

  if (flags_ & PbrShader::Flag::BaseColorTexture) {
    shader_->bindBaseColorTexture(*matCache.baseColorTexture);
  }

  if (flags_ & PbrShader::Flag::NoneRoughnessMetallicTexture) {
    shader_->bindMetallicRoughnessTexture(
        *matCache.noneRoughnessMetallicTexture);
  }

  if (flags_ & PbrShader::Flag::NormalTexture) {
    shader_->bindNormalTexture(*matCache.normalTexture);
    shader_->setNormalTextureScale(matCache.normalTextureScale);
  }

  if (flags_ & PbrShader::Flag::EmissiveTexture) {
    shader_->bindEmissiveTexture(*matCache.emissiveTexture);
  }

  if (flags_ & PbrShader::Flag::TextureTransformation) {
    shader_->setTextureMatrix(matCache.textureMatrix);
  }

  // clearcoat data
  if (flags_ & PbrShader::Flag::ClearCoatLayer) {
    (*shader_)
        .setClearCoatFactor(matCache.clearCoat.factor)
        .setClearCoatRoughness(matCache.clearCoat.roughnessFactor)
        .setClearCoatNormalTextureScale(matCache.clearCoat.normalTextureScale);
    if (flags_ >= PbrShader::Flag::ClearCoatTexture) {
      shader_->bindClearCoatFactorTexture(*matCache.clearCoat.texture);
    }
    if (flags_ >= PbrShader::Flag::ClearCoatRoughnessTexture) {
      shader_->bindClearCoatRoughnessTexture(
          *matCache.clearCoat.roughnessTexture);
    }
    if (flags_ >= PbrShader::Flag::ClearCoatNormalTexture) {
      shader_->bindClearCoatNormalTexture(*matCache.clearCoat.normalTexture);
    }
  }

  // specular layer data
  if (flags_ & PbrShader::Flag::SpecularLayer) {
    (*shader_)
        .setSpecularLayerFactor(matCache.specularLayer.factor)
        .setSpecularLayerColorFactor(matCache.specularLayer.colorFactor);

    if (flags_ >= PbrShader::Flag::SpecularLayerTexture) {
      shader_->bindSpecularLayerTexture(*matCache.specularLayer.texture);
    }
    if (flags_ >= PbrShader::Flag::SpecularLayerColorTexture) {
      shader_->bindSpecularLayerColorTexture(
          *matCache.specularLayer.colorTexture);
    }
  }

  // anisotropy layer data
  if (flags_ & PbrShader::Flag::AnisotropyLayer) {
    (*shader_)
        .setAnisotropyLayerFactor(matCache.anisotropyLayer.factor)
        .setAnisotropyLayerRotation(matCache.anisotropyLayer.rotation);

    if (flags_ >= PbrShader::Flag::AnisotropyLayerTexture) {
      shader_->bindAnisotropyLayerTexture(*matCache.anisotropyLayer.texture);
    }
  }

  // setup image based lighting for the shader
  if (flags_ & PbrShader::Flag::ImageBasedLighting) {
    CORRADE_INTERNAL_ASSERT(pbrIbl_);
    shader_->bindIrradianceCubeMap(  // TODO: HDR Color
        pbrIbl_->getIrradianceMap().getTexture(CubeMap::TextureType::Color));
    shader_->bindBrdfLUT(pbrIbl_->getBrdfLookupTable());
    shader_->bindPrefilteredMap(
        // TODO: HDR Color
        pbrIbl_->getPrefilteredMap().getTexture(CubeMap::TextureType::Color));
    shader_->setPrefilteredMapMipLevels(
        pbrIbl_->getPrefilteredMap().getMipmapLevels());
  }

  if (flags_ & PbrShader::Flag::ShadowsVSM) {
    CORRADE_INTERNAL_ASSERT(shadowMapManger_ && shadowMapKeys_);
    CORRADE_ASSERT(shadowMapKeys_->size() <= 3,
                   "PbrDrawable::draw: the number of shadow maps exceeds the "
                   "maximum (current it is 3).", );
    for (int iShadow = 0; iShadow < shadowMapKeys_->size(); ++iShadow) {
      Mn::Resource<CubeMap> shadowMap =
          (*shadowMapManger_).get<CubeMap>((*shadowMapKeys_)[iShadow]);

      CORRADE_INTERNAL_ASSERT(shadowMap);

      if (flags_ & PbrShader::Flag::ShadowsVSM) {
        shader_->bindPointShadowMap(
            iShadow,
            shadowMap->getTexture(CubeMap::TextureType::VarianceShadowMap));
      }
    }
  }

  shader_->draw(getMesh());

  // Reset winding direction
  if (normalDet < 0) {
    Mn::GL::Renderer::setFrontFace(
        Mn::GL::Renderer::FrontFace::CounterClockWise);
  }

  // WE stopped supporting doubleSided material due to lighting artifacts on
  // hard edges. See comments at the beginning of this function.
  /*
  if ((flags_ & PbrShader::Flag::DoubleSided) && !glIsEnabled(GL_CULL_FACE))
  { Mn::GL::Renderer::enable(Mn::GL::Renderer::Feature::FaceCulling);
  }
  */
}  // namespace gfx

Mn::ResourceKey PbrDrawable::getShaderKey(Mn::UnsignedInt lightCount,
                                          PbrShader::Flags flags) const {
  return Corrade::Utility::formatString(
      SHADER_KEY_TEMPLATE, lightCount,
      static_cast<PbrShader::Flags::UnderlyingType>(flags));
}

PbrDrawable& PbrDrawable::updateShader() {
  unsigned int lightCount = lightSetup_->size();
  if (!shader_ || shader_->lightCount() != lightCount ||
      shader_->flags() != flags_) {
    // if the number of lights or flags have changed, we need to fetch a
    // compatible shader
    shader_ = shaderManager_.get<Mn::GL::AbstractShaderProgram, PbrShader>(
        getShaderKey(lightCount, flags_));

    // if no shader with desired number of lights and flags exists, create
    // one
    if (!shader_) {
      shaderManager_.set<Mn::GL::AbstractShaderProgram>(
          shader_.key(), new PbrShader{flags_, lightCount},
          Mn::ResourceDataState::Final, Mn::ResourcePolicy::ReferenceCounted);
    }

    CORRADE_INTERNAL_ASSERT(shader_ && shader_->lightCount() == lightCount &&
                            shader_->flags() == flags_);
  }

  return *this;
}

// update every light's color, intensity, range etc.
PbrDrawable& PbrDrawable::updateShaderLightParameters() {
  // light range has been initialized to Mn::Constants::inf()
  // in the PbrShader's constructor.
  // No need to reset it at this point.
  std::vector<Mn::Color3> colors;
  colors.reserve(lightSetup_->size());
  for (unsigned int iLight = 0; iLight < lightSetup_->size(); ++iLight) {
    // Note: the light color MUST take the intensity into account
    colors.emplace_back((*lightSetup_)[iLight].color);
  }

  shader_->setLightColors(colors);
  return *this;
}

// update light direction (or position) in *world* space to the shader
PbrDrawable& PbrDrawable::updateShaderLightDirectionParameters(
    const Mn::Matrix4& transformationMatrix,
    Mn::SceneGraph::Camera3D& camera) {
  std::vector<Mn::Vector4> lightPositions;
  lightPositions.reserve(lightSetup_->size());

  const Mn::Matrix4 cameraMatrix = camera.cameraMatrix();
  for (unsigned int iLight = 0; iLight < lightSetup_->size(); ++iLight) {
    const auto& lightInfo = (*lightSetup_)[iLight];
    Mn::Vector4 pos = getLightPositionRelativeToWorld(
        lightInfo, transformationMatrix, cameraMatrix);
    // flip directional lights to faciliate faster, non-forking calc in
    // shader.  Leave non-directional lights unchanged (w==1)
    pos *= (pos[3] * 2) - 1;
    lightPositions.emplace_back(pos);
  }

  shader_->setLightVectors(lightPositions);

  return *this;
}

void PbrDrawable::setShadowData(ShadowMapManager& manager,
                                ShadowMapKeys& keys,
                                PbrShader::Flag shadowFlag) {
  // sanity check first
  CORRADE_ASSERT(shadowFlag == PbrShader::Flag::ShadowsVSM,
                 "PbrDrawable::setShadowData(): the shadow flag can only be "
                 "ShadowsVSM.", );

  shadowMapManger_ = &manager;
  shadowMapKeys_ = &keys;
  flags_ |= shadowFlag;
}

}  // namespace gfx
}  // namespace esp
