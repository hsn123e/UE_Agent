#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "IHttpRouter.h"

class IHttpRouter;

class FUEAgentBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// Execute a tool by name and return HTTP-style status code + JSON body (as string).
	// Intended for in-editor UI usage.
	bool ExecuteToolForUI(const FString& ToolName, const TSharedPtr<FJsonObject>& Input, int32& OutStatusCode, FString& OutBodyJson);

private:
	void StartServer();
	void StopServer();
	void RegisterRoutes();

	bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleTools(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleToolCall(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	bool HandleTool_ProjectGetDirectory(const FHttpResultCallback& OnComplete);
	bool HandleTool_ProjectGetName(const FHttpResultCallback& OnComplete);
	bool HandleTool_EditorGetSelectedActors(const FHttpResultCallback& OnComplete);
	bool HandleTool_EditorGetWorldInfo(const FHttpResultCallback& OnComplete);
	bool HandleTool_EditorCreateBlueprintFromSelectedActor(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_EditorGetSelectedActorDetails(const FHttpResultCallback& OnComplete);
	bool HandleTool_EditorSetSelectedSkeletalAnimation(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_LevelSpawnActor(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_LevelSaveCurrent(const FHttpResultCallback& OnComplete);
	bool HandleTool_AssetSearch(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_AssetCreateBlueprint(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_BlueprintGetGraphT3D(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_BlueprintPasteT3D(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgCreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgAddWidget(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgSetText(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgSetProperties(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgScaffoldLayout(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgBindEvent(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgBindProperty(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgUnbind(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgListWidgets(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);
	bool HandleTool_UmgCompile(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete);

	TSharedPtr<IHttpRouter> Router;
	uint16 Port = 30020;
	FString Token;
};
