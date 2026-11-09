// Copyright 2026 Nwiro. All Rights Reserved.
#include "NwiroStyle.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateStyle.h"

TSharedPtr<FSlateStyleSet> FNwiroStyle::StyleInstance = nullptr;

void FNwiroStyle::Initialize()
{
    if (!StyleInstance.IsValid())
    {
        StyleInstance = MakeShareable(new FSlateStyleSet("NwiroStyle"));

        FString ContentDir = IPluginManager::Get().FindPlugin(TEXT("Nwiro"))->GetBaseDir() / TEXT("Resources");
        StyleInstance->SetContentRoot(ContentDir);

        FString IconPath = StyleInstance->RootToContentDir(TEXT("Icon128"), TEXT(".png"));
        StyleInstance->Set("Nwiro.Logo", new FSlateImageBrush(IconPath, FVector2D(16, 16)));

        // Dedicated level-editor toolbar button icon ("Open Nwiro Pro").
        FString ToolbarIconPath = StyleInstance->RootToContentDir(TEXT("ToolbarIcon"), TEXT(".png"));
        StyleInstance->Set("Nwiro.ToolbarIcon", new FSlateImageBrush(ToolbarIconPath, FVector2D(20, 20)));

        FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
    }
}

void FNwiroStyle::Shutdown()
{
    if (StyleInstance.IsValid())
    {
        FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
        StyleInstance.Reset();
    }
}
