// macOS-only glue: attach a CAMetalLayer to the GLFW cocoa window's view
// and hand it to the RHI for surface creation.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

extern "C" void* infinityMetalLayerForCocoaWindow(void* nsWindow) {
  NSWindow* window = (__bridge NSWindow*)nsWindow;
  NSView* view = [window contentView];
  if (![view.layer isKindOfClass:[CAMetalLayer class]]) {
    [view setWantsLayer:YES];
    view.layer = [CAMetalLayer layer];
  }
  return (__bridge void*)view.layer;
}
