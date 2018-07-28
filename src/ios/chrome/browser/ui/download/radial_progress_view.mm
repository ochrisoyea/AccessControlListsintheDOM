// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/download/radial_progress_view.h"

#import <QuartzCore/QuartzCore.h>

#include "base/mac/foundation_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@interface RadialProgressView ()

// CALayer that backs this view up. Serves as progress track.
@property(nonatomic, readonly) CAShapeLayer* trackLayer;

// CALayer added as a sublayer for self.layer. Serves as progress.
@property(nonatomic, readonly) CAShapeLayer* progressLayer;

@end

@implementation RadialProgressView

@synthesize progress = _progress;
@synthesize lineWidth = _lineWidth;
@synthesize progressTintColor = _progressTintColor;
@synthesize trackTintColor = _trackTintColor;
@synthesize progressLayer = _progressLayer;

#pragma mark - UIView overrides

+ (Class)layerClass {
  return [CAShapeLayer class];
}

- (void)setBounds:(CGRect)bounds {
  [super setBounds:bounds];

  // progressPathWithEndAngle: relies on self.bounds and must be updated here.
  // Track starts at 12 o'clock and ends at 12 o'clock.
  self.trackLayer.path =
      [self progressPathWithEndAngle:M_PI * 2 - M_PI_2].CGPath;
}

- (void)willMoveToSuperview:(UIView*)newSuperview {
  if (newSuperview && !self.progressLayer.superlayer) {
    self.trackLayer.fillColor = [UIColor clearColor].CGColor;
    self.trackLayer.strokeColor = self.trackTintColor.CGColor;
    self.trackLayer.lineWidth = self.lineWidth;

    [self.trackLayer addSublayer:self.progressLayer];
  }
}

#pragma mark - Public

- (void)setProgress:(float)progress {
  if (_progress != progress) {
    _progress = progress;
    self.progressLayer.path =
        [self progressPathWithEndAngle:M_PI * 2 * progress - M_PI_2].CGPath;
  }
}

#pragma mark - Private

// Returns Bezier path for drawing radial progress or track. Start angle is
// always 12 o'clock.
- (UIBezierPath*)progressPathWithEndAngle:(CGFloat)endAngleInRadians {
  CGPoint center =
      CGPointMake(CGRectGetMidX(self.bounds), CGRectGetMidY(self.bounds));
  CGFloat radius = CGRectGetWidth(self.bounds) / 2 - self.lineWidth;
  return [UIBezierPath bezierPathWithArcCenter:center
                                        radius:radius
                                    startAngle:-M_PI_2
                                      endAngle:endAngleInRadians
                                     clockwise:YES];
}

- (CAShapeLayer*)trackLayer {
  return base::mac::ObjCCastStrict<CAShapeLayer>(self.layer);
}

- (CAShapeLayer*)progressLayer {
  if (!_progressLayer) {
    _progressLayer = [CAShapeLayer layer];
    _progressLayer.fillColor = [UIColor clearColor].CGColor;
    _progressLayer.strokeColor = self.progressTintColor.CGColor;
    _progressLayer.lineWidth = self.lineWidth;
  }
  return _progressLayer;
}

@end