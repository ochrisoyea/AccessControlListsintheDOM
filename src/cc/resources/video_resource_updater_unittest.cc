// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/resources/video_resource_updater.h"

#include <stddef.h>
#include <stdint.h>

#include "base/bind.h"
#include "cc/resources/resource_provider.h"
#include "cc/test/fake_layer_tree_frame_sink.h"
#include "cc/test/fake_output_surface_client.h"
#include "cc/test/fake_resource_provider.h"
#include "components/viz/test/fake_output_surface.h"
#include "components/viz/test/test_web_graphics_context_3d.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "media/base/video_frame.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace cc {
namespace {

class WebGraphicsContext3DUploadCounter : public viz::TestWebGraphicsContext3D {
 public:
  void texSubImage2D(GLenum target,
                     GLint level,
                     GLint xoffset,
                     GLint yoffset,
                     GLsizei width,
                     GLsizei height,
                     GLenum format,
                     GLenum type,
                     const void* pixels) override {
    ++upload_count_;
  }

  void texStorage2DEXT(GLenum target,
                       GLint levels,
                       GLuint internalformat,
                       GLint width,
                       GLint height) override {
  }

  GLuint createTexture() override {
    ++created_texture_count_;
    return TestWebGraphicsContext3D::createTexture();
  }

  void deleteTexture(GLuint texture) override {
    --created_texture_count_;
    TestWebGraphicsContext3D::deleteTexture(texture);
  }

  void deleteTextures(GLsizei count, const GLuint* ids) override {
    created_texture_count_ -= count;
    TestWebGraphicsContext3D::deleteTextures(count, ids);
  }

  int UploadCount() { return upload_count_; }
  void ResetUploadCount() { upload_count_ = 0; }

  int TextureCreationCount() { return created_texture_count_; }
  void ResetTextureCreationCount() { created_texture_count_ = 0; }

 private:
  int upload_count_;
  int created_texture_count_;
};

class VideoResourceUpdaterTest : public testing::Test {
 protected:
  VideoResourceUpdaterTest() {
    std::unique_ptr<WebGraphicsContext3DUploadCounter> context3d(
        new WebGraphicsContext3DUploadCounter());

    context3d_ = context3d.get();
    context3d_->set_support_texture_storage(true);

    context_provider_ = viz::TestContextProvider::Create(std::move(context3d));
    context_provider_->BindToCurrentThread();
  }

  // testing::Test implementation.
  void SetUp() override {
    testing::Test::SetUp();
    layer_tree_frame_sink_software_ = FakeLayerTreeFrameSink::CreateSoftware();
    resource_provider3d_ =
        FakeResourceProvider::CreateLayerTreeResourceProvider(
            context_provider_.get());
    resource_provider_software_ =
        FakeResourceProvider::CreateLayerTreeResourceProvider(nullptr);
  }

  std::unique_ptr<VideoResourceUpdater> CreateUpdaterForHardware(
      bool use_stream_video_draw_quad = false) {
    return std::make_unique<VideoResourceUpdater>(
        context_provider_.get(), nullptr, resource_provider3d_.get(),
        use_stream_video_draw_quad, /*use_gpu_memory_buffer_resources=*/false,
        /*use_r16_texture=*/use_r16_texture_);
  }

  std::unique_ptr<VideoResourceUpdater> CreateUpdaterForSoftware() {
    return std::make_unique<VideoResourceUpdater>(
        nullptr, layer_tree_frame_sink_software_.get(),
        resource_provider_software_.get(),
        /*use_stream_video_draw_quad=*/false,
        /*use_gpu_memory_buffer_resources=*/false,
        /*use_r16_texture=*/false);
  }

  scoped_refptr<media::VideoFrame> CreateTestYUVVideoFrame() {
    const int kDimension = 10;
    gfx::Size size(kDimension, kDimension);
    static uint8_t y_data[kDimension * kDimension] = {0};
    static uint8_t u_data[kDimension * kDimension / 2] = {0};
    static uint8_t v_data[kDimension * kDimension / 2] = {0};

    scoped_refptr<media::VideoFrame> video_frame =
        media::VideoFrame::WrapExternalYuvData(
            media::PIXEL_FORMAT_I422,  // format
            size,                      // coded_size
            gfx::Rect(size),           // visible_rect
            size,                      // natural_size
            size.width(),              // y_stride
            size.width() / 2,          // u_stride
            size.width() / 2,          // v_stride
            y_data,                    // y_data
            u_data,                    // u_data
            v_data,                    // v_data
            base::TimeDelta());        // timestamp
    EXPECT_TRUE(video_frame);
    return video_frame;
  }

  scoped_refptr<media::VideoFrame> CreateWonkyTestYUVVideoFrame() {
    const int kDimension = 10;
    const int kYWidth = kDimension + 5;
    const int kUWidth = (kYWidth + 1) / 2 + 200;
    const int kVWidth = (kYWidth + 1) / 2 + 1;
    static uint8_t y_data[kYWidth * kDimension] = {0};
    static uint8_t u_data[kUWidth * kDimension] = {0};
    static uint8_t v_data[kVWidth * kDimension] = {0};

    scoped_refptr<media::VideoFrame> video_frame =
        media::VideoFrame::WrapExternalYuvData(
            media::PIXEL_FORMAT_I422,                 // format
            gfx::Size(kYWidth, kDimension),           // coded_size
            gfx::Rect(2, 0, kDimension, kDimension),  // visible_rect
            gfx::Size(kDimension, kDimension),        // natural_size
            -kYWidth,                                 // y_stride (negative)
            kUWidth,                                  // u_stride
            kVWidth,                                  // v_stride
            y_data + kYWidth * (kDimension - 1),      // y_data
            u_data,                                   // u_data
            v_data,                                   // v_data
            base::TimeDelta());                       // timestamp
    EXPECT_TRUE(video_frame);
    return video_frame;
  }

  scoped_refptr<media::VideoFrame> CreateTestHighBitFrame() {
    const int kDimension = 10;
    gfx::Size size(kDimension, kDimension);

    scoped_refptr<media::VideoFrame> video_frame(media::VideoFrame::CreateFrame(
        media::PIXEL_FORMAT_YUV420P10, size, gfx::Rect(size), size,
        base::TimeDelta()));
    EXPECT_TRUE(video_frame);
    return video_frame;
  }

  void SetReleaseSyncToken(const gpu::SyncToken& sync_token) {
    release_sync_token_ = sync_token;
  }

  scoped_refptr<media::VideoFrame> CreateTestHardwareVideoFrame(
      media::VideoPixelFormat format,
      unsigned target) {
    const int kDimension = 10;
    gfx::Size size(kDimension, kDimension);

    gpu::Mailbox mailbox;
    mailbox.name[0] = 51;

    gpu::MailboxHolder mailbox_holders[media::VideoFrame::kMaxPlanes] = {
        gpu::MailboxHolder(mailbox, kMailboxSyncToken, target)};
    scoped_refptr<media::VideoFrame> video_frame =
        media::VideoFrame::WrapNativeTextures(
            format, mailbox_holders,
            base::Bind(&VideoResourceUpdaterTest::SetReleaseSyncToken,
                       base::Unretained(this)),
            size,                // coded_size
            gfx::Rect(size),     // visible_rect
            size,                // natural_size
            base::TimeDelta());  // timestamp
    EXPECT_TRUE(video_frame);
    return video_frame;
  }

  scoped_refptr<media::VideoFrame> CreateTestRGBAHardwareVideoFrame() {
    return CreateTestHardwareVideoFrame(media::PIXEL_FORMAT_ARGB,
                                        GL_TEXTURE_2D);
  }

  scoped_refptr<media::VideoFrame> CreateTestStreamTextureHardwareVideoFrame(
      bool needs_copy) {
    scoped_refptr<media::VideoFrame> video_frame = CreateTestHardwareVideoFrame(
        media::PIXEL_FORMAT_ARGB, GL_TEXTURE_EXTERNAL_OES);
    video_frame->metadata()->SetBoolean(
        media::VideoFrameMetadata::COPY_REQUIRED, needs_copy);
    return video_frame;
  }

  scoped_refptr<media::VideoFrame> CreateTestYuvHardwareVideoFrame(
      media::VideoPixelFormat format,
      size_t num_textures,
      unsigned target) {
    const int kDimension = 10;
    gfx::Size size(kDimension, kDimension);

    gpu::MailboxHolder mailbox_holders[media::VideoFrame::kMaxPlanes];
    for (size_t i = 0; i < num_textures; ++i) {
      gpu::Mailbox mailbox;
      mailbox.name[0] = 50 + 1;
      mailbox_holders[i] =
          gpu::MailboxHolder(mailbox, kMailboxSyncToken, target);
    }
    scoped_refptr<media::VideoFrame> video_frame =
        media::VideoFrame::WrapNativeTextures(
            format, mailbox_holders,
            base::Bind(&VideoResourceUpdaterTest::SetReleaseSyncToken,
                       base::Unretained(this)),
            size,                // coded_size
            gfx::Rect(size),     // visible_rect
            size,                // natural_size
            base::TimeDelta());  // timestamp
    EXPECT_TRUE(video_frame);
    return video_frame;
  }

  static const gpu::SyncToken kMailboxSyncToken;

  WebGraphicsContext3DUploadCounter* context3d_;
  scoped_refptr<viz::TestContextProvider> context_provider_;
  std::unique_ptr<FakeLayerTreeFrameSink> layer_tree_frame_sink_software_;
  std::unique_ptr<LayerTreeResourceProvider> resource_provider3d_;
  std::unique_ptr<LayerTreeResourceProvider> resource_provider_software_;
  gpu::SyncToken release_sync_token_;
  bool use_r16_texture_ = false;
};

const gpu::SyncToken VideoResourceUpdaterTest::kMailboxSyncToken =
    gpu::SyncToken(gpu::CommandBufferNamespace::GPU_IO,
                   gpu::CommandBufferId::FromUnsafeValue(0x123),
                   7);

TEST_F(VideoResourceUpdaterTest, SoftwareFrame) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestYUVVideoFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
}

TEST_F(VideoResourceUpdaterTest, HighBitFrameNoF16) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestHighBitFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
}

class VideoResourceUpdaterTestWithF16 : public VideoResourceUpdaterTest {
 public:
  VideoResourceUpdaterTestWithF16() : VideoResourceUpdaterTest() {
    context3d_->set_support_texture_half_float_linear(true);
  }
};

TEST_F(VideoResourceUpdaterTestWithF16, HighBitFrame) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestHighBitFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
  EXPECT_NEAR(resources.multiplier, 2.0, 0.1);
  EXPECT_NEAR(resources.offset, 0.5, 0.1);

  // Create the resource again, to test the path where the
  // resources are cached.
  VideoFrameExternalResources resources2 =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources2.type);
  EXPECT_NEAR(resources2.multiplier, 2.0, 0.1);
  EXPECT_NEAR(resources2.offset, 0.5, 0.1);
}

class VideoResourceUpdaterTestWithR16 : public VideoResourceUpdaterTest {
 public:
  VideoResourceUpdaterTestWithR16() : VideoResourceUpdaterTest() {
    use_r16_texture_ = true;
    context3d_->set_support_texture_norm16(true);
  }
};

TEST_F(VideoResourceUpdaterTestWithR16, HighBitFrame) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestHighBitFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);

  // Max 10-bit values as read by a sampler.
  double max_10bit_value = ((1 << 10) - 1) / 65535.0;
  EXPECT_NEAR(resources.multiplier * max_10bit_value, 1.0, 0.0001);
  EXPECT_NEAR(resources.offset, 0.0, 0.1);

  // Create the resource again, to test the path where the
  // resources are cached.
  VideoFrameExternalResources resources2 =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources2.type);
  EXPECT_NEAR(resources2.multiplier * max_10bit_value, 1.0, 0.0001);
  EXPECT_NEAR(resources2.offset, 0.0, 0.1);
}

TEST_F(VideoResourceUpdaterTest, HighBitFrameSoftwareCompositor) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForSoftware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestHighBitFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
}

TEST_F(VideoResourceUpdaterTest, WonkySoftwareFrame) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  scoped_refptr<media::VideoFrame> video_frame = CreateWonkyTestYUVVideoFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
}

TEST_F(VideoResourceUpdaterTest, WonkySoftwareFrameSoftwareCompositor) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForSoftware();
  scoped_refptr<media::VideoFrame> video_frame = CreateWonkyTestYUVVideoFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
}

TEST_F(VideoResourceUpdaterTest, ReuseResource) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestYUVVideoFrame();
  video_frame->set_timestamp(base::TimeDelta::FromSeconds(1234));

  // Allocate the resources for a YUV video frame.
  context3d_->ResetUploadCount();
  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
  EXPECT_EQ(3u, resources.resources.size());
  EXPECT_EQ(3u, resources.release_callbacks.size());
  // Expect exactly three texture uploads, one for each plane.
  EXPECT_EQ(3, context3d_->UploadCount());

  // Simulate the ResourceProvider releasing the resources back to the video
  // updater.
  for (auto& release_callback : resources.release_callbacks)
    std::move(release_callback).Run(gpu::SyncToken(), false);

  // Allocate resources for the same frame.
  context3d_->ResetUploadCount();
  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
  EXPECT_EQ(3u, resources.resources.size());
  EXPECT_EQ(3u, resources.release_callbacks.size());
  // The data should be reused so expect no texture uploads.
  EXPECT_EQ(0, context3d_->UploadCount());
}

TEST_F(VideoResourceUpdaterTest, ReuseResourceNoDelete) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestYUVVideoFrame();
  video_frame->set_timestamp(base::TimeDelta::FromSeconds(1234));

  // Allocate the resources for a YUV video frame.
  context3d_->ResetUploadCount();
  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
  EXPECT_EQ(3u, resources.resources.size());
  EXPECT_EQ(3u, resources.release_callbacks.size());
  // Expect exactly three texture uploads, one for each plane.
  EXPECT_EQ(3, context3d_->UploadCount());

  // Allocate resources for the same frame.
  context3d_->ResetUploadCount();
  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
  EXPECT_EQ(3u, resources.resources.size());
  EXPECT_EQ(3u, resources.release_callbacks.size());
  // The data should be reused so expect no texture uploads.
  EXPECT_EQ(0, context3d_->UploadCount());
}

TEST_F(VideoResourceUpdaterTest, SoftwareFrameSoftwareCompositor) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForSoftware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestYUVVideoFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
}

TEST_F(VideoResourceUpdaterTest, ReuseResourceSoftwareCompositor) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForSoftware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestYUVVideoFrame();
  video_frame->set_timestamp(base::TimeDelta::FromSeconds(1234));

  // Allocate the resources for a software video frame.
  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ(1u, resources.release_callbacks.size());
  // Expect exactly one allocated shared bitmap.
  EXPECT_EQ(1u, layer_tree_frame_sink_software_->shared_bitmaps().size());
  auto shared_bitmaps = layer_tree_frame_sink_software_->shared_bitmaps();

  // Simulate the ResourceProvider releasing the resource back to the video
  // updater.
  std::move(resources.release_callbacks[0]).Run(gpu::SyncToken(), false);

  // Allocate resources for the same frame.
  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ(1u, resources.release_callbacks.size());

  // Ensure that the same shared bitmap was reused.
  EXPECT_EQ(layer_tree_frame_sink_software_->shared_bitmaps(), shared_bitmaps);
}

TEST_F(VideoResourceUpdaterTest, ReuseResourceNoDeleteSoftwareCompositor) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForSoftware();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestYUVVideoFrame();
  video_frame->set_timestamp(base::TimeDelta::FromSeconds(1234));

  // Allocate the resources for a software video frame.
  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ(1u, resources.release_callbacks.size());
  // Expect exactly one allocated shared bitmap.
  EXPECT_EQ(1u, layer_tree_frame_sink_software_->shared_bitmaps().size());
  auto shared_bitmaps = layer_tree_frame_sink_software_->shared_bitmaps();

  // Allocate resources for the same frame.
  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ(1u, resources.release_callbacks.size());

  // Ensure that the same shared bitmap was reused.
  EXPECT_EQ(layer_tree_frame_sink_software_->shared_bitmaps(), shared_bitmaps);
}

TEST_F(VideoResourceUpdaterTest, CreateForHardwarePlanes) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();

  scoped_refptr<media::VideoFrame> video_frame =
      CreateTestRGBAHardwareVideoFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ(1u, resources.release_callbacks.size());

  video_frame = CreateTestYuvHardwareVideoFrame(media::PIXEL_FORMAT_I420, 3,
                                                GL_TEXTURE_RECTANGLE_ARB);

  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
  EXPECT_EQ(3u, resources.resources.size());
  EXPECT_EQ(3u, resources.release_callbacks.size());
  EXPECT_FALSE(resources.resources[0].read_lock_fences_enabled);
  EXPECT_FALSE(resources.resources[1].read_lock_fences_enabled);
  EXPECT_FALSE(resources.resources[2].read_lock_fences_enabled);

  video_frame = CreateTestYuvHardwareVideoFrame(media::PIXEL_FORMAT_I420, 3,
                                                GL_TEXTURE_RECTANGLE_ARB);
  video_frame->metadata()->SetBoolean(
      media::VideoFrameMetadata::READ_LOCK_FENCES_ENABLED, true);

  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_TRUE(resources.resources[0].read_lock_fences_enabled);
  EXPECT_TRUE(resources.resources[1].read_lock_fences_enabled);
  EXPECT_TRUE(resources.resources[2].read_lock_fences_enabled);
}

TEST_F(VideoResourceUpdaterTest, CreateForHardwarePlanes_StreamTexture) {
  // Note that |use_stream_video_draw_quad| is true for this test.
  std::unique_ptr<VideoResourceUpdater> updater =
      CreateUpdaterForHardware(true);
  context3d_->ResetTextureCreationCount();
  scoped_refptr<media::VideoFrame> video_frame =
      CreateTestStreamTextureHardwareVideoFrame(false);

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::STREAM_TEXTURE, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ((GLenum)GL_TEXTURE_EXTERNAL_OES,
            resources.resources[0].mailbox_holder.texture_target);
  EXPECT_EQ(1u, resources.release_callbacks.size());
  EXPECT_EQ(0, context3d_->TextureCreationCount());

  // A copied stream texture should return an RGBA resource in a new
  // GL_TEXTURE_2D texture.
  context3d_->ResetTextureCreationCount();
  video_frame = CreateTestStreamTextureHardwareVideoFrame(true);
  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ((GLenum)GL_TEXTURE_2D,
            resources.resources[0].mailbox_holder.texture_target);
  EXPECT_EQ(1u, resources.release_callbacks.size());
  EXPECT_EQ(1, context3d_->TextureCreationCount());
}

TEST_F(VideoResourceUpdaterTest, CreateForHardwarePlanes_TextureQuad) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  context3d_->ResetTextureCreationCount();
  scoped_refptr<media::VideoFrame> video_frame =
      CreateTestStreamTextureHardwareVideoFrame(false);

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGBA_PREMULTIPLIED, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ((GLenum)GL_TEXTURE_EXTERNAL_OES,
            resources.resources[0].mailbox_holder.texture_target);
  EXPECT_EQ(1u, resources.release_callbacks.size());
  EXPECT_EQ(0, context3d_->TextureCreationCount());
}

// Passthrough the sync token returned by the compositor if we don't have an
// existing release sync token.
TEST_F(VideoResourceUpdaterTest, PassReleaseSyncToken) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();

  const gpu::SyncToken sync_token(gpu::CommandBufferNamespace::GPU_IO,
                                  gpu::CommandBufferId::FromUnsafeValue(0x123),
                                  123);

  {
    scoped_refptr<media::VideoFrame> video_frame =
        CreateTestRGBAHardwareVideoFrame();

    VideoFrameExternalResources resources =
        updater->CreateExternalResourcesFromVideoFrame(video_frame);

    ASSERT_EQ(resources.release_callbacks.size(), 1u);
    std::move(resources.release_callbacks[0]).Run(sync_token, false);
  }

  EXPECT_EQ(release_sync_token_, sync_token);
}

// Generate new sync token because video frame has an existing sync token.
TEST_F(VideoResourceUpdaterTest, GenerateReleaseSyncToken) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();

  const gpu::SyncToken sync_token1(gpu::CommandBufferNamespace::GPU_IO,
                                   gpu::CommandBufferId::FromUnsafeValue(0x123),
                                   123);

  const gpu::SyncToken sync_token2(gpu::CommandBufferNamespace::GPU_IO,
                                   gpu::CommandBufferId::FromUnsafeValue(0x234),
                                   234);

  {
    scoped_refptr<media::VideoFrame> video_frame =
        CreateTestRGBAHardwareVideoFrame();

    VideoFrameExternalResources resources1 =
        updater->CreateExternalResourcesFromVideoFrame(video_frame);
    ASSERT_EQ(resources1.release_callbacks.size(), 1u);
    std::move(resources1.release_callbacks[0]).Run(sync_token1, false);

    VideoFrameExternalResources resources2 =
        updater->CreateExternalResourcesFromVideoFrame(video_frame);
    ASSERT_EQ(resources2.release_callbacks.size(), 1u);
    std::move(resources2.release_callbacks[0]).Run(sync_token2, false);
  }

  EXPECT_TRUE(release_sync_token_.HasData());
  EXPECT_NE(release_sync_token_, sync_token1);
  EXPECT_NE(release_sync_token_, sync_token2);
}

// Pass mailbox sync token as is if no GL operations are performed before frame
// resources are handed off to the compositor.
TEST_F(VideoResourceUpdaterTest, PassMailboxSyncToken) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();

  scoped_refptr<media::VideoFrame> video_frame =
      CreateTestRGBAHardwareVideoFrame();

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);

  ASSERT_EQ(resources.resources.size(), 1u);
  EXPECT_TRUE(resources.resources[0].mailbox_holder.sync_token.HasData());
  EXPECT_EQ(resources.resources[0].mailbox_holder.sync_token,
            kMailboxSyncToken);
}

// Generate new sync token for compositor when copying the texture.
TEST_F(VideoResourceUpdaterTest, GenerateSyncTokenOnTextureCopy) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();

  scoped_refptr<media::VideoFrame> video_frame =
      CreateTestStreamTextureHardwareVideoFrame(true /* needs_copy */);

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);

  ASSERT_EQ(resources.resources.size(), 1u);
  EXPECT_TRUE(resources.resources[0].mailbox_holder.sync_token.HasData());
  EXPECT_NE(resources.resources[0].mailbox_holder.sync_token,
            kMailboxSyncToken);
}

// NV12 VideoFrames backed by a single native texture can be sampled out
// by GL as RGB. To use them as HW overlays we need to know the format
// of the underlying buffer, that is YUV_420_BIPLANAR.
TEST_F(VideoResourceUpdaterTest, CreateForHardwarePlanes_SingleNV12) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  context3d_->ResetTextureCreationCount();
  scoped_refptr<media::VideoFrame> video_frame = CreateTestHardwareVideoFrame(
      media::PIXEL_FORMAT_NV12, GL_TEXTURE_EXTERNAL_OES);

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGB, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ((GLenum)GL_TEXTURE_EXTERNAL_OES,
            resources.resources[0].mailbox_holder.texture_target);
  EXPECT_EQ(gfx::BufferFormat::YUV_420_BIPLANAR,
            resources.resources[0].buffer_format);

  video_frame = CreateTestYuvHardwareVideoFrame(media::PIXEL_FORMAT_NV12, 1,
                                                GL_TEXTURE_RECTANGLE_ARB);
  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::RGB, resources.type);
  EXPECT_EQ(1u, resources.resources.size());
  EXPECT_EQ((GLenum)GL_TEXTURE_RECTANGLE_ARB,
            resources.resources[0].mailbox_holder.texture_target);
  EXPECT_EQ(gfx::BufferFormat::YUV_420_BIPLANAR,
            resources.resources[0].buffer_format);

  EXPECT_EQ(0, context3d_->TextureCreationCount());
}

TEST_F(VideoResourceUpdaterTest, CreateForHardwarePlanes_DualNV12) {
  std::unique_ptr<VideoResourceUpdater> updater = CreateUpdaterForHardware();
  context3d_->ResetTextureCreationCount();
  scoped_refptr<media::VideoFrame> video_frame =
      CreateTestYuvHardwareVideoFrame(media::PIXEL_FORMAT_NV12, 2,
                                      GL_TEXTURE_EXTERNAL_OES);

  VideoFrameExternalResources resources =
      updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
  EXPECT_EQ(2u, resources.resources.size());
  EXPECT_EQ(2u, resources.release_callbacks.size());
  EXPECT_EQ((GLenum)GL_TEXTURE_EXTERNAL_OES,
            resources.resources[0].mailbox_holder.texture_target);
  // |updater| doesn't set |buffer_format| in this case.
  EXPECT_EQ(gfx::BufferFormat::RGBA_8888, resources.resources[0].buffer_format);

  video_frame = CreateTestYuvHardwareVideoFrame(media::PIXEL_FORMAT_NV12, 2,
                                                GL_TEXTURE_RECTANGLE_ARB);
  resources = updater->CreateExternalResourcesFromVideoFrame(video_frame);
  EXPECT_EQ(VideoFrameResourceType::YUV, resources.type);
  EXPECT_EQ(2u, resources.resources.size());
  EXPECT_EQ((GLenum)GL_TEXTURE_RECTANGLE_ARB,
            resources.resources[0].mailbox_holder.texture_target);
  EXPECT_EQ(gfx::BufferFormat::RGBA_8888, resources.resources[0].buffer_format);

  EXPECT_EQ(0, context3d_->TextureCreationCount());
}

}  // namespace
}  // namespace cc
