const kScaleEpsilon = 0.0001;
const kOffsetEpsilon = 2;

const centerX = window.innerWidth / 2;
const centerY = window.innerHeight / 2;

// TODO(bokan): Add more variations (speed, anchor location, etc.) but it's
// difficult right now as pinchBy is badly broken on desktops:
// https://crbug.com/787615.
function runTest(t, testCase) {
  return new Promise(t.step_func((resolve, reject) => {
    internals.setPageScaleFactor(testCase.startingScale);
    internals.setVisualViewportOffset(
        window.innerWidth * (1 - (1 / testCase.startingScale)) / 2,
        window.innerHeight * (1 - (1 / testCase.startingScale)) / 2);

    // Ensure the compositor knows the starting scale and offset.
    waitForCompositorCommit().then(t.step_func(() => {
      chrome.gpuBenchmarking.pinchBy(
          testCase.scale, centerX, centerY, t.step_func(() => {
        const expectedScale = testCase.startingScale * testCase.scale;
        assert_approx_equals(
            window.visualViewport.scale,
            expectedScale,
            kScaleEpsilon,
            testCase.msg + " has correct page scale factor.");
        assert_approx_equals(
            window.visualViewport.offsetLeft,
            window.innerWidth * (1 - (1 / expectedScale)) / 2,
            kOffsetEpsilon,
            testCase.msg + " has correct visual viewport offsetLeft.");
        assert_approx_equals(
            window.visualViewport.offsetTop,
            window.innerHeight * (1 - (1 / expectedScale)) / 2,
            kOffsetEpsilon,
            testCase.msg + " has correct visual viewport offsetTop.");
        resolve();
      }), testCase.speed, testCase.gestureSource);
    }));
  }));
}

async function runAllTestCases(t, testCases) {
  for (const testCase of testCases) {
    await runTest(t, testCase);
  }
}
