#import <Cocoa/Cocoa.h>

void setDarkModeAppearance() {
    @autoreleasepool {
        [NSApp setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
    }
}
