// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

class FNwiroStyle
{
public:
    static void Initialize();
    static void Shutdown();
    static TSharedPtr<ISlateStyle> Get() { return StyleInstance; }
    static FName GetStyleSetName() { return "NwiroStyle"; }

private:
    static TSharedPtr<FSlateStyleSet> StyleInstance;
};
