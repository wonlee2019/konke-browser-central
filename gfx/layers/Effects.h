/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_LAYERS_EFFECTS_H
#define MOZILLA_LAYERS_EFFECTS_H

#include "mozilla/Assertions.h"  // for MOZ_ASSERT, etc
#include "mozilla/RefPtr.h"      // for RefPtr, already_AddRefed, etc
#include "mozilla/gfx/Matrix.h"  // for Matrix4x4
#include "mozilla/gfx/Point.h"   // for IntSize
#include "mozilla/gfx/Rect.h"    // for Rect
#include "mozilla/gfx/Types.h"   // for SamplingFilter, etc
#include "mozilla/layers/CompositorTypes.h"  // for EffectTypes, etc
#include "mozilla/layers/TextureHost.h"      // for CompositingRenderTarget, etc
#include "mozilla/mozalloc.h"                // for operator delete, etc
#include "nscore.h"                          // for nsACString
#include "mozilla/EnumeratedArray.h"

namespace mozilla {
namespace layers {

/**
 * Effects and effect chains are used by the compositor API (see Compositor.h).
 * An effect chain represents a rendering method, for example some shader and
 * the data required for that shader to run. An effect is some component of the
 * chain and its data.
 *
 * An effect chain consists of a primary effect - how the 'texture' memory
 * should be interpreted (RGBA, BGRX, YCBCR, etc.) - and any number of secondary
 * effects
 * - any way in which rendering can be changed, e.g., applying a mask layer.
 *
 * During the rendering process, an effect chain is created by the layer being
 * rendered and the primary effect is added by the compositable host. Secondary
 * effects may be added by the layer or compositable. The effect chain is passed
 * to the compositor by the compositable host as a parameter to DrawQuad.
 */

struct TexturedEffect;

struct Effect {
  NS_INLINE_DECL_REFCOUNTING(Effect)

  explicit Effect(EffectTypes aType) : mType(aType) {}

  EffectTypes mType;

  virtual TexturedEffect* AsTexturedEffect() { return nullptr; }
  virtual void PrintInfo(std::stringstream& aStream, const char* aPrefix) = 0;

 protected:
  virtual ~Effect() = default;
};

// Render from a texture
struct TexturedEffect : public Effect {
  TexturedEffect(EffectTypes aType, TextureSource* aTexture,
                 bool aPremultiplied, gfx::SamplingFilter aSamplingFilter)
      : Effect(aType),
        mTextureCoords(0, 0, 1.0f, 1.0f),
        mTexture(aTexture),
        mPremultiplied(aPremultiplied),
        mSamplingFilter(aSamplingFilter) {}

  TexturedEffect* AsTexturedEffect() override { return this; }
  virtual const char* Name() = 0;
  void PrintInfo(std::stringstream& aStream, const char* aPrefix) override;

  gfx::Rect mTextureCoords;
  TextureSource* mTexture;
  bool mPremultiplied;
  gfx::SamplingFilter mSamplingFilter;
};

// Support an alpha mask.
struct EffectMask : public Effect {
  EffectMask(TextureSource* aMaskTexture, gfx::IntSize aSize,
             const gfx::Matrix4x4& aMaskTransform)
      : Effect(EffectTypes::MASK),
        mMaskTexture(aMaskTexture),
        mSize(aSize),
        mMaskTransform(aMaskTransform) {}

  void PrintInfo(std::stringstream& aStream, const char* aPrefix) override;

  TextureSource* mMaskTexture;
  gfx::IntSize mSize;
  gfx::Matrix4x4 mMaskTransform;
};

struct EffectBlendMode : public Effect {
  explicit EffectBlendMode(gfx::CompositionOp aBlendMode)
      : Effect(EffectTypes::BLEND_MODE), mBlendMode(aBlendMode) {}

  virtual const char* Name() { return "EffectBlendMode"; }
  void PrintInfo(std::stringstream& aStream, const char* aPrefix) override;

  gfx::CompositionOp mBlendMode;
};

// Render to a render target rather than the screen.
struct EffectRenderTarget : public TexturedEffect {
  explicit EffectRenderTarget(CompositingRenderTarget* aRenderTarget)
      : TexturedEffect(EffectTypes::RENDER_TARGET, aRenderTarget, true,
                       gfx::SamplingFilter::LINEAR),
        mRenderTarget(aRenderTarget) {}

  const char* Name() override { return "EffectRenderTarget"; }
  void PrintInfo(std::stringstream& aStream, const char* aPrefix) override;

  RefPtr<CompositingRenderTarget> mRenderTarget;

 protected:
  EffectRenderTarget(EffectTypes aType, CompositingRenderTarget* aRenderTarget)
      : TexturedEffect(aType, aRenderTarget, true, gfx::SamplingFilter::LINEAR),
        mRenderTarget(aRenderTarget) {}
};

// Render to a render target rather than the screen.
struct EffectColorMatrix : public Effect {
  explicit EffectColorMatrix(gfx::Matrix5x4 aMatrix)
      : Effect(EffectTypes::COLOR_MATRIX), mColorMatrix(aMatrix) {}

  void PrintInfo(std::stringstream& aStream, const char* aPrefix) override;
  const gfx::Matrix5x4 mColorMatrix;
};

struct EffectRGB : public TexturedEffect {
  EffectRGB(TextureSource* aTexture, bool aPremultiplied,
            gfx::SamplingFilter aSamplingFilter, bool aFlipped = false)
      : TexturedEffect(EffectTypes::RGB, aTexture, aPremultiplied,
                       aSamplingFilter) {}

  const char* Name() override { return "EffectRGB"; }
};

struct EffectYCbCr : public TexturedEffect {
  EffectYCbCr(TextureSource* aSource, gfx::YUVColorSpace aYUVColorSpace,
              gfx::ColorRange aColorRange, gfx::ColorDepth aColorDepth,
              gfx::SamplingFilter aSamplingFilter)
      : TexturedEffect(EffectTypes::YCBCR, aSource, false, aSamplingFilter),
        mYUVColorSpace(aYUVColorSpace),
        mColorRange(aColorRange),
        mColorDepth(aColorDepth) {}

  const char* Name() override { return "EffectYCbCr"; }

  gfx::YUVColorSpace mYUVColorSpace;
  gfx::ColorRange mColorRange;
  gfx::ColorDepth mColorDepth;
};

struct EffectNV12 : public EffectYCbCr {
  EffectNV12(TextureSource* aSource, gfx::YUVColorSpace aYUVColorSpace,
             gfx::ColorRange aColorRange, gfx::ColorDepth aColorDepth,
             gfx::SamplingFilter aSamplingFilter)
      : EffectYCbCr(aSource, aYUVColorSpace, aColorRange, aColorDepth,
                    aSamplingFilter) {
    mType = EffectTypes::NV12;
  }

  const char* Name() override { return "EffectNV12"; }
};

struct EffectComponentAlpha : public TexturedEffect {
  EffectComponentAlpha(TextureSource* aOnBlack, TextureSource* aOnWhite,
                       gfx::SamplingFilter aSamplingFilter)
      : TexturedEffect(EffectTypes::COMPONENT_ALPHA, nullptr, false,
                       aSamplingFilter),
        mOnBlack(aOnBlack),
        mOnWhite(aOnWhite) {}

  const char* Name() override { return "EffectComponentAlpha"; }

  TextureSource* mOnBlack;
  TextureSource* mOnWhite;
};

struct EffectSolidColor : public Effect {
  explicit EffectSolidColor(const gfx::DeviceColor& aColor)
      : Effect(EffectTypes::SOLID_COLOR), mColor(aColor) {}

  void PrintInfo(std::stringstream& aStream, const char* aPrefix) override;

  gfx::DeviceColor mColor;
};

struct EffectChain {
  EffectChain() : mLayerRef(nullptr) {}
  explicit EffectChain(void* aLayerRef) : mLayerRef(aLayerRef) {}

  RefPtr<Effect> mPrimaryEffect;
  EnumeratedArray<EffectTypes, EffectTypes::MAX_SECONDARY, RefPtr<Effect>>
      mSecondaryEffects;
  void* mLayerRef;  //!< For LayerScope logging
};

/**
 * Create a Textured effect corresponding to aFormat and using
 * aSource as the (first) texture source.
 *
 * Note that aFormat can be different form aSource->GetFormat if, we are
 * creating an effect that takes several texture sources (like with YCBCR
 * where aFormat would be FORMAT_YCBCR and each texture source would be
 * a one-channel A8 texture)
 */
inline already_AddRefed<TexturedEffect> CreateTexturedEffect(
    gfx::SurfaceFormat aFormat, TextureSource* aSource,
    const gfx::SamplingFilter aSamplingFilter, bool isAlphaPremultiplied) {
  MOZ_ASSERT(aSource);
  RefPtr<TexturedEffect> result;
  switch (aFormat) {
    case gfx::SurfaceFormat::B8G8R8A8:
    case gfx::SurfaceFormat::B8G8R8X8:
    case gfx::SurfaceFormat::R8G8B8X8:
    case gfx::SurfaceFormat::R5G6B5_UINT16:
    case gfx::SurfaceFormat::R8G8B8A8:
      result = new EffectRGB(aSource, isAlphaPremultiplied, aSamplingFilter);
      break;
    case gfx::SurfaceFormat::YUV:
    case gfx::SurfaceFormat::NV12:
    case gfx::SurfaceFormat::P010:
    case gfx::SurfaceFormat::P016:
      MOZ_ASSERT_UNREACHABLE(
          "gfx::SurfaceFormat::YUV/NV12/P010/P016 is invalid");
      break;
    default:
      NS_WARNING("unhandled program type");
      break;
  }

  return result.forget();
}

inline already_AddRefed<TexturedEffect> CreateTexturedEffect(
    TextureHost* aHost, TextureSource* aSource,
    const gfx::SamplingFilter aSamplingFilter, bool isAlphaPremultiplied) {
  MOZ_ASSERT(aHost);
  MOZ_ASSERT(aSource);

  RefPtr<TexturedEffect> result;

  switch (aHost->GetReadFormat()) {
    case gfx::SurfaceFormat::YUV:
      result = new EffectYCbCr(aSource, aHost->GetYUVColorSpace(),
                               aHost->GetColorRange(), aHost->GetColorDepth(),
                               aSamplingFilter);
      break;
    case gfx::SurfaceFormat::NV12:
    case gfx::SurfaceFormat::P010:
    case gfx::SurfaceFormat::P016:
      result = new EffectNV12(aSource, aHost->GetYUVColorSpace(),
                              aHost->GetColorRange(), aHost->GetColorDepth(),
                              aSamplingFilter);
      break;
    default:
      result = CreateTexturedEffect(aHost->GetReadFormat(), aSource,
                                    aSamplingFilter, isAlphaPremultiplied);
      break;
  }
  return result.forget();
}

/**
 * Create a textured effect based on aSource format and the presence of
 * aSourceOnWhite.
 *
 * aSourceOnWhite can be null.
 */
inline already_AddRefed<TexturedEffect> CreateTexturedEffect(
    TextureSource* aSource, TextureSource* aSourceOnWhite,
    const gfx::SamplingFilter aSamplingFilter, bool isAlphaPremultiplied) {
  MOZ_ASSERT(aSource);
  if (aSourceOnWhite) {
    MOZ_ASSERT(aSource->GetFormat() == gfx::SurfaceFormat::R8G8B8X8 ||
               aSource->GetFormat() == gfx::SurfaceFormat::B8G8R8X8);
    MOZ_ASSERT(aSource->GetFormat() == aSourceOnWhite->GetFormat());
    return MakeAndAddRef<EffectComponentAlpha>(aSource, aSourceOnWhite,
                                               aSamplingFilter);
  }

  return CreateTexturedEffect(aSource->GetFormat(), aSource, aSamplingFilter,
                              isAlphaPremultiplied);
}

/**
 * Create a textured effect based on aSource format.
 *
 * This version excudes the possibility of component alpha.
 */
inline already_AddRefed<TexturedEffect> CreateTexturedEffect(
    TextureSource* aTexture, const gfx::SamplingFilter aSamplingFilter) {
  return CreateTexturedEffect(aTexture, nullptr, aSamplingFilter, true);
}

}  // namespace layers
}  // namespace mozilla

#endif