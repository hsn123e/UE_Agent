#include "UEAgentBridgeModule.h"
#include "UEAgentBridgeSettings.h"

#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"

#include "Framework/Application/SlateApplication.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#include "Styling/AppStyle.h"

#include "InputCoreTypes.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"

#include "Async/Async.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_K2.h"

#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ContentWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/RichTextBlock.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "Components/SkeletalMeshComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimationAsset.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompilerModule.h"
#include "FileHelpers.h"
#include "Engine/Selection.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

static const FName UEAgentBridgeTabName(TEXT("UEAgentBridge"));

static FString HttpResponseBodyToString(const FHttpServerResponse& Response)
{
	if (Response.Body.Num() <= 0)
	{
		return FString();
	}
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Response.Body.GetData());
	FUTF8ToTCHAR Convert(Data, Response.Body.Num());
	return FString(Convert.Get(), Convert.Length());
}

static bool TryParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObj)
{
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, OutObj) && OutObj.IsValid();
}

static bool TryExtractFirstJsonObject(const FString& Text, FString& OutJson)
{
	int32 Start = INDEX_NONE;
	for (int32 i = 0; i < Text.Len(); i++)
	{
		if (Text[i] == TCHAR('{'))
		{
			Start = i;
			break;
		}
	}
	if (Start == INDEX_NONE)
	{
		return false;
	}

	int32 Depth = 0;
	bool bInString = false;
	bool bEscaped = false;
	for (int32 i = Start; i < Text.Len(); i++)
	{
		const TCHAR C = Text[i];
		if (bInString)
		{
			if (bEscaped)
			{
				bEscaped = false;
				continue;
			}
			if (C == TCHAR('\\'))
			{
				bEscaped = true;
				continue;
			}
			if (C == TCHAR('"'))
			{
				bInString = false;
				continue;
			}
			continue;
		}

		if (C == TCHAR('"'))
		{
			bInString = true;
			continue;
		}

		if (C == TCHAR('{'))
		{
			Depth++;
		}
		else if (C == TCHAR('}'))
		{
			Depth--;
			if (Depth == 0)
			{
				OutJson = Text.Mid(Start, i - Start + 1);
				return true;
			}
		}
	}

	return false;
}

static FString GetAgentSystemPrompt()
{
	// Keep short-ish to avoid huge prompts; enough for reliable tool usage.
	return TEXT(
		"You are an Unreal Editor agent. You can call tools to modify the current Unreal project.\n"
		"Available tools (toolName):\n"
		"- project.get_directory, project.get_name\n"
		"- editor.get_selected_actors, editor.get_selected_actor_details, editor.get_world_info\n"
		"- editor.create_blueprint_from_selected_actor, editor.set_selected_skeletal_animation\n"
		"- level.save_current, level.spawn_actor\n"
		"- asset.search, asset.create_blueprint\n"
		"- blueprint.get_graph_t3d, blueprint.paste_t3d\n"
		"- umg.create_widget_blueprint, umg.add_widget, umg.set_text, umg.set_properties, umg.scaffold_layout\n"
		"- umg.bind_event, umg.bind_property, umg.unbind, umg.list_widgets, umg.compile\n"
		"\n"
		"Protocol:\n"
		"- When the user asks you to DO something in the editor, you must keep working until it is done.\n"
		"- Do NOT ask the user to confirm each step. Do NOT wait for the user between steps.\n"
		"- Only ask a question if you are truly blocked and cannot safely proceed.\n"
		"- Prefer tool calls over explanations. Save explanations for the very end.\n"
		"- If you need a tool, respond with ONLY a single JSON object:\n"
		"  {\"type\":\"tool_call\",\"toolName\":\"...\",\"input\":{...}}\n"
		"- After a tool call, you will receive a user message containing JSON:\n"
		"  {\"type\":\"tool_result\",\"toolName\":\"...\",\"statusCode\":200,\"body\":{...}}\n"
		"- When you are done and no more tools are needed, respond with normal text summarizing what you did (step-by-step).\n"
		"\n"
		"Guidelines:\n"
		"- The user may write Arabic; reply in the user's language.\n"
		"- If the user refers to the selected actor, call editor.get_selected_actors or editor.get_selected_actor_details first.\n"
		"- asset.search defaults to /Game when packagePath is omitted.\n"
	);
}

class SUEAgentBridgePanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUEAgentBridgePanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, class FUEAgentBridgeModule* InModule)
	{
		Module = InModule;

		UUEAgentBridgeSettings* Settings = GetMutableDefault<UUEAgentBridgeSettings>();
		Provider = Settings->Provider;
		BaseUrl = Settings->BaseUrl;
		ApiKey = Settings->ApiKey;
		Model = Settings->Model;
		MaxSteps = Settings->MaxSteps;
		Temperature = Settings->Temperature;

		InitProviderPresets();

		LoadConversations();
		if (Conversations.Num() == 0)
		{
			CreateNewConversation(TEXT("Chat 1"));
		}

		ChildSlot
		[
			SNew(SBorder)
			.Padding(8)
			[
				SNew(SSplitter)
				+ SSplitter::Slot()
				.Value(0.25f)
				[
					BuildConversationPane()
				]
				+ SSplitter::Slot()
				.Value(0.75f)
				[
					BuildChatPane()
				]
			]
		];

		RefreshConversationList();
		SyncTranscriptFromConversation();
	}

	private:
	struct FConversationListItem
	{
		FString Id;
		FString Title;
		FDateTime UpdatedAt;
	};

	struct FAgentMessage
	{
		FString Role;
		FString Content;
	};

	struct FConversation
	{
		FString Id;
		FString Title;
		FDateTime UpdatedAt;
		TArray<FAgentMessage> Messages;
	};

	FString GetConversationsPath() const
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEAgentBridge"), TEXT("conversations.json"));
	}

	int32 FindConversationIndexById(const FString& Id) const
	{
		for (int32 i = 0; i < Conversations.Num(); i++)
		{
			if (Conversations[i].Id == Id)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	FConversation* GetActiveConversation()
	{
		const int32 Idx = FindConversationIndexById(ActiveConversationId);
		return Idx != INDEX_NONE ? &Conversations[Idx] : nullptr;
	}

	static FString MakeConversationTitleFromPrompt(const FString& Prompt)
	{
		FString S = Prompt;
		S.ReplaceInline(TEXT("\r"), TEXT(""));
		S.ReplaceInline(TEXT("\n"), TEXT(" "));
		S.TrimStartAndEndInline();
		while (S.Contains(TEXT("  ")))
		{
			S.ReplaceInline(TEXT("  "), TEXT(" "));
		}

		// Avoid tool_result JSON blobs becoming titles.
		if (S.StartsWith(TEXT("{")))
		{
			return FString();
		}

		const int32 MaxLen = 42;
		if (S.Len() > MaxLen)
		{
			return S.Left(MaxLen - 1) + TEXT("…");
		}
		return S;
	}

	void MaybeAutoTitleActiveConversation(const FString& Prompt)
	{
		FConversation* C = GetActiveConversation();
		if (!C)
		{
			return;
		}

		const bool bNeedsTitle = C->Title.IsEmpty() || C->Title.StartsWith(TEXT("Chat "), ESearchCase::IgnoreCase) || C->Title.Equals(TEXT("Chat"), ESearchCase::IgnoreCase);
		if (!bNeedsTitle)
		{
			return;
		}

		const FString NewTitle = MakeConversationTitleFromPrompt(Prompt);
		if (NewTitle.IsEmpty())
		{
			return;
		}

		C->Title = NewTitle;
		C->UpdatedAt = FDateTime::UtcNow();
		RefreshConversationList();
		SaveConversations();
	}

	void SyncTranscriptFromConversation()
	{
		Transcript.Empty();
		if (TranscriptBox.IsValid())
		{
			TranscriptBox->SetText(FText::GetEmpty());
		}

		if (FConversation* C = GetActiveConversation())
		{
			for (const FAgentMessage& M : C->Messages)
			{
				if (M.Role == TEXT("system"))
				{
					continue;
				}
				if (M.Role == TEXT("assistant"))
				{
					AppendTranscript(TEXT("[assistant] ") + M.Content);
					continue;
				}

				// Render tool_result messages nicely (they are stored as "user" role for the agent loop)
				TSharedPtr<FJsonObject> Obj;
				FString Type;
				if (TryParseJsonObject(M.Content, Obj) && Obj.IsValid() && Obj->TryGetStringField(TEXT("type"), Type) && Type.Equals(TEXT("tool_result"), ESearchCase::IgnoreCase))
				{
					// Show full tool_result payload for debugging and step-by-step visibility.
					AppendTranscript(TEXT("[tool_result] ") + M.Content.Left(12000));
					continue;
				}

				AppendTranscript(TEXT("[user] ") + M.Content);
			}
		}
	}

	void LoadConversations()
	{
		Conversations.Reset();
		ActiveConversationId.Reset();

		const FString Path = GetConversationsPath();
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path) || Text.IsEmpty())
		{
			return;
		}

		TSharedPtr<FJsonObject> Root;
		if (!TryParseJsonObject(Text, Root))
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Root->TryGetArrayField(TEXT("conversations"), Items) || !Items)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& V : *Items)
		{
			const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}

			FConversation C;
			Obj->TryGetStringField(TEXT("id"), C.Id);
			Obj->TryGetStringField(TEXT("title"), C.Title);

			FString UpdatedStr;
			if (Obj->TryGetStringField(TEXT("updatedAt"), UpdatedStr))
			{
				FDateTime::ParseIso8601(*UpdatedStr, C.UpdatedAt);
			}

			const TArray<TSharedPtr<FJsonValue>>* MsgArr = nullptr;
			if (Obj->TryGetArrayField(TEXT("messages"), MsgArr) && MsgArr)
			{
				for (const TSharedPtr<FJsonValue>& MV : *MsgArr)
				{
					const TSharedPtr<FJsonObject> MO = MV.IsValid() ? MV->AsObject() : nullptr;
					if (!MO.IsValid())
					{
						continue;
					}
					FAgentMessage M;
					MO->TryGetStringField(TEXT("role"), M.Role);
					MO->TryGetStringField(TEXT("content"), M.Content);
					if (!M.Role.IsEmpty())
					{
						C.Messages.Add(M);
					}
				}
			}

			if (!C.Id.IsEmpty())
			{
				Conversations.Add(MoveTemp(C));
			}
		}

		if (Conversations.Num() > 0)
		{
			ActiveConversationId = Conversations[0].Id;
			Messages = Conversations[0].Messages;
		}
		RefreshConversationList();
	}

	void SaveConversations()
	{
		const FString Path = GetConversationsPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Items;

		for (const FConversation& C : Conversations)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("id"), C.Id);
			Obj->SetStringField(TEXT("title"), C.Title);
			Obj->SetStringField(TEXT("updatedAt"), C.UpdatedAt.ToIso8601());

			TArray<TSharedPtr<FJsonValue>> MsgArr;
			for (const FAgentMessage& M : C.Messages)
			{
				auto MO = MakeShared<FJsonObject>();
				MO->SetStringField(TEXT("role"), M.Role);
				MO->SetStringField(TEXT("content"), M.Content);
				MsgArr.Add(MakeShared<FJsonValueObject>(MO));
			}
			Obj->SetArrayField(TEXT("messages"), MsgArr);

			Items.Add(MakeShared<FJsonValueObject>(Obj));
		}

		Root->SetArrayField(TEXT("conversations"), Items);

		FString Out;
		auto Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		FFileHelper::SaveStringToFile(Out, *Path);
	}

	void CreateNewConversation(const FString& Title)
	{
		FConversation C;
		C.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		C.Title = Title;
		C.UpdatedAt = FDateTime::UtcNow();
		C.Messages.Add({TEXT("system"), GetAgentSystemPrompt()});

		Conversations.Insert(C, 0);
		ActiveConversationId = C.Id;
		Messages = C.Messages;
		RefreshConversationList();

		SaveConversations();
		SyncTranscriptFromConversation();
	}

	void DeleteActiveConversation()
	{
		const int32 Idx = FindConversationIndexById(ActiveConversationId);
		if (Idx == INDEX_NONE)
		{
			return;
		}

		Conversations.RemoveAt(Idx);
		if (Conversations.Num() == 0)
		{
			CreateNewConversation(TEXT("Chat 1"));
			return;
		}

		ActiveConversationId = Conversations[0].Id;
		if (FConversation* C = GetActiveConversation())
		{
			Messages = C->Messages;
		}
		RefreshConversationList();

		SaveConversations();
		SyncTranscriptFromConversation();
	}

	void RefreshConversationList()
	{
		ConversationListItems.Reset();
		for (const FConversation& C : Conversations)
		{
			auto Item = MakeShared<FConversationListItem>();
			Item->Id = C.Id;
			Item->Title = C.Title;
			Item->UpdatedAt = C.UpdatedAt;
			ConversationListItems.Add(Item);
		}

		if (ConversationListView.IsValid())
		{
			ConversationListView->RequestListRefresh();
		}

		SelectedConversationItem.Reset();
		for (const TSharedPtr<FConversationListItem>& Item : ConversationListItems)
		{
			if (Item.IsValid() && Item->Id == ActiveConversationId)
			{
				SelectedConversationItem = Item;
				break;
			}
		}
		if (!SelectedConversationItem.IsValid() && ConversationListItems.Num() > 0)
		{
			ActiveConversationId = ConversationListItems[0]->Id;
			SelectedConversationItem = ConversationListItems[0];
		}
		if (ConversationListView.IsValid() && SelectedConversationItem.IsValid())
		{
			bSuppressConversationSelectionChanged = true;
			ConversationListView->SetSelection(SelectedConversationItem);
			bSuppressConversationSelectionChanged = false;
		}
	}

	struct FProviderPreset
	{
		FString Label;
		EUEAgentProvider Provider = EUEAgentProvider::OpenAICompatible;
		FString BaseUrl;
		bool bAllowEditBaseUrl = false;
		TArray<FString> SuggestedModels;
	};

	void InitProviderPresets()
	{
		ProviderPresets.Reset();

		// Ollama Cloud (native /chat endpoint)
		{
			FProviderPreset P;
			P.Label = TEXT("Ollama Cloud");
			P.Provider = EUEAgentProvider::OllamaCloud;
			P.BaseUrl = TEXT("https://ollama.com/api");
			P.bAllowEditBaseUrl = false;
			P.SuggestedModels = { TEXT("gpt-oss:120b") };
			ProviderPresets.Add(MoveTemp(P));
		}

		// Ollama Local (native /chat endpoint)
		{
			FProviderPreset P;
			P.Label = TEXT("Ollama (Local)");
			P.Provider = EUEAgentProvider::OllamaCloud;
			P.BaseUrl = TEXT("http://localhost:11434/api");
			P.bAllowEditBaseUrl = false;
			ProviderPresets.Add(MoveTemp(P));
		}

		// OpenAI-compatible presets (common providers)
		auto AddOpenAICompat = [this](const FString& Label, const FString& Url, const TArray<FString>& Models)
		{
			FProviderPreset P;
			P.Label = Label;
			P.Provider = EUEAgentProvider::OpenAICompatible;
			P.BaseUrl = Url;
			P.bAllowEditBaseUrl = false;
			P.SuggestedModels = Models;
			ProviderPresets.Add(MoveTemp(P));
		};

		AddOpenAICompat(TEXT("OpenAI"), TEXT("https://api.openai.com/v1"), { TEXT("gpt-4o-mini"), TEXT("gpt-4.1-mini") });
		AddOpenAICompat(TEXT("OpenRouter"), TEXT("https://openrouter.ai/api/v1"), {});
		AddOpenAICompat(TEXT("Groq"), TEXT("https://api.groq.com/openai/v1"), {});
		AddOpenAICompat(TEXT("Together"), TEXT("https://api.together.xyz/v1"), {});
		AddOpenAICompat(TEXT("Fireworks"), TEXT("https://api.fireworks.ai/inference/v1"), {});
		AddOpenAICompat(TEXT("DeepInfra"), TEXT("https://api.deepinfra.com/v1/openai"), {});
		AddOpenAICompat(TEXT("LM Studio (Local)"), TEXT("http://localhost:1234/v1"), {});
		AddOpenAICompat(TEXT("Ollama (Local OpenAI-compatible)"), TEXT("http://localhost:11434/v1"), {});

		// Custom
		{
			FProviderPreset P;
			P.Label = TEXT("Custom (OpenAI-compatible)");
			P.Provider = EUEAgentProvider::OpenAICompatible;
			P.BaseUrl = BaseUrl.IsEmpty() ? TEXT("http://localhost:11434/v1") : BaseUrl;
			P.bAllowEditBaseUrl = true;
			ProviderPresets.Add(MoveTemp(P));
		}

		ProviderPresetOptions.Reset();
		for (const FProviderPreset& P : ProviderPresets)
		{
			ProviderPresetOptions.Add(MakeShared<FString>(P.Label));
		}

		const int32 CustomIdx = ProviderPresets.Num() - 1;

		// Choose best matching preset for current settings
		SelectedProviderPresetLabel.Reset();
		for (int32 i = 0; i < ProviderPresets.Num(); i++)
		{
			if (ProviderPresets[i].Provider == Provider && ProviderPresets[i].BaseUrl.Equals(BaseUrl, ESearchCase::IgnoreCase))
			{
				SelectedProviderPresetLabel = ProviderPresetOptions[i];
				break;
			}
		}
		if (!SelectedProviderPresetLabel.IsValid() && ProviderPresetOptions.Num() > 0)
		{
			SelectedProviderPresetLabel = ProviderPresetOptions.IsValidIndex(CustomIdx) ? ProviderPresetOptions[CustomIdx] : ProviderPresetOptions[0];
			ApplyPresetByIndex(ProviderPresetOptions.IsValidIndex(CustomIdx) ? CustomIdx : 0);
		}
		else
		{
			const int32 SelIdx = GetSelectedPresetIndex();
			if (SelIdx != INDEX_NONE)
			{
				ApplySuggestedModelsFromPreset(ProviderPresets[SelIdx]);
			}
		}
	}

	int32 GetSelectedPresetIndex() const
	{
		if (!SelectedProviderPresetLabel.IsValid())
		{
			return INDEX_NONE;
		}
		for (int32 i = 0; i < ProviderPresetOptions.Num(); i++)
		{
			if (ProviderPresetOptions[i] == SelectedProviderPresetLabel)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	void ApplySuggestedModelsFromPreset(const FProviderPreset& Preset)
	{
		ModelOptions.Reset();
		for (const FString& M : Preset.SuggestedModels)
		{
			ModelOptions.Add(MakeShared<FString>(M));
		}
		if (!Model.IsEmpty())
		{
			ModelOptions.Insert(MakeShared<FString>(Model), 0);
		}

		SelectedModelLabel.Reset();
		for (const TSharedPtr<FString>& Opt : ModelOptions)
		{
			if (Opt.IsValid() && *Opt == Model)
			{
				SelectedModelLabel = Opt;
				break;
			}
		}
		if (!SelectedModelLabel.IsValid() && ModelOptions.Num() > 0)
		{
			SelectedModelLabel = ModelOptions[0];
		}
		if (ModelCombo.IsValid())
		{
			ModelCombo->RefreshOptions();
			if (SelectedModelLabel.IsValid())
			{
				ModelCombo->SetSelectedItem(SelectedModelLabel);
			}
		}
	}

	void ApplyPresetByIndex(int32 Index)
	{
		if (!ProviderPresets.IsValidIndex(Index))
		{
			return;
		}
		const FProviderPreset& P = ProviderPresets[Index];
		Provider = P.Provider;
		BaseUrl = P.BaseUrl;
		if (BaseUrlBox.IsValid())
		{
			BaseUrlBox->SetText(FText::FromString(BaseUrl));
		}
		ApplySuggestedModelsFromPreset(P);
	}

	void OnProviderPresetChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type)
	{
		SelectedProviderPresetLabel = NewValue;
		const int32 Idx = GetSelectedPresetIndex();
		if (Idx != INDEX_NONE)
		{
			ApplyPresetByIndex(Idx);
		}
	}

	FText GetProviderPresetText() const
	{
		return FText::FromString(SelectedProviderPresetLabel.IsValid() ? *SelectedProviderPresetLabel : TEXT(""));
	}

	void OnModelSelected(TSharedPtr<FString> NewValue, ESelectInfo::Type)
	{
		SelectedModelLabel = NewValue;
		if (NewValue.IsValid())
		{
			Model = *NewValue;
			if (ModelBox.IsValid())
			{
				ModelBox->SetText(FText::FromString(Model));
			}
		}
	}

	FText GetModelText() const
	{
		return FText::FromString(SelectedModelLabel.IsValid() ? *SelectedModelLabel : Model);
	}

	FReply OnRefreshModelsClicked()
	{
		if (BaseUrlBox.IsValid())
		{
			BaseUrl = BaseUrlBox->GetText().ToString();
		}
		if (ApiKeyBox.IsValid())
		{
			ApiKey = ApiKeyBox->GetText().ToString();
		}

		FString Url = BaseUrl;
		Url.TrimStartAndEndInline();
		while (Url.EndsWith(TEXT("/")))
		{
			Url.LeftChopInline(1);
		}

		if (Provider == EUEAgentProvider::OllamaCloud)
		{
			Url += TEXT("/tags");
		}
		else
		{
			Url += TEXT("/models");
		}

		TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
		Req->SetVerb(TEXT("GET"));
		Req->SetURL(Url);
		Req->SetHeader(TEXT("Accept"), TEXT("application/json"));
		if (!ApiKey.IsEmpty())
		{
			Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		}

		const TWeakPtr<SUEAgentBridgePanel> SelfWeak = StaticCastSharedRef<SUEAgentBridgePanel>(AsShared());
		Req->OnProcessRequestComplete().BindLambda([SelfWeak](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			if (!SelfWeak.IsValid())
			{
				return;
			}
			SelfWeak.Pin()->OnModelsResponse(Resp, bOk);
		});

		if (!Req->ProcessRequest())
		{
			AppendTranscript(TEXT("[models] could not start request"));
		}
		return FReply::Handled();
	}

	void OnModelsResponse(FHttpResponsePtr Resp, bool bOk)
	{
		if (!bOk || !Resp.IsValid())
		{
			AppendTranscript(TEXT("[models] request failed"));
			return;
		}

		const int32 Code = Resp->GetResponseCode();
		if (Code < 200 || Code >= 300)
		{
			AppendTranscript(FString::Printf(TEXT("[models] http %d"), Code));
			AppendTranscript(Resp->GetContentAsString().Left(2000));
			return;
		}

		TSharedPtr<FJsonObject> Root;
		if (!TryParseJsonObject(Resp->GetContentAsString(), Root))
		{
			AppendTranscript(TEXT("[models] invalid JSON"));
			return;
		}

		TArray<FString> Names;
		if (Provider == EUEAgentProvider::OllamaCloud)
		{
			const TArray<TSharedPtr<FJsonValue>>* ModelsArr = nullptr;
			if (Root->TryGetArrayField(TEXT("models"), ModelsArr) && ModelsArr)
			{
				for (const TSharedPtr<FJsonValue>& V : *ModelsArr)
				{
					const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
					if (!Obj.IsValid())
					{
						continue;
					}
					FString Name;
					if (Obj->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
					{
						Names.Add(Name);
					}
				}
			}
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* DataArr = nullptr;
			if (Root->TryGetArrayField(TEXT("data"), DataArr) && DataArr)
			{
				for (const TSharedPtr<FJsonValue>& V : *DataArr)
				{
					const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
					if (!Obj.IsValid())
					{
						continue;
					}
					FString Id;
					if (Obj->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
					{
						Names.Add(Id);
					}
				}
			}
		}

		Names.Sort();
		ModelOptions.Reset();
		for (const FString& N : Names)
		{
			ModelOptions.Add(MakeShared<FString>(N));
		}

		if (ModelCombo.IsValid())
		{
			ModelCombo->RefreshOptions();
		}
		AppendTranscript(FString::Printf(TEXT("[models] loaded %d models"), ModelOptions.Num()));
	}

	TSharedRef<SWidget> BuildConversationPane()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("Chats")))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("New")))
					.OnClicked(this, &SUEAgentBridgePanel::OnNewConversationClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Delete")))
					.OnClicked(this, &SUEAgentBridgePanel::OnDeleteConversationClicked)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(ConversationListView, SListView<TSharedPtr<FConversationListItem>>)
				.ListItemsSource(&ConversationListItems)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SUEAgentBridgePanel::OnGenerateConversationRow)
				.OnSelectionChanged(this, &SUEAgentBridgePanel::OnConversationSelected)
			];
	}

	TSharedRef<ITableRow> OnGenerateConversationRow(TSharedPtr<FConversationListItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
	{
		const FString Title = Item.IsValid() ? Item->Title : FString();
		const FString Updated = Item.IsValid() ? Item->UpdatedAt.ToString(TEXT("%Y-%m-%d %H:%M")) : FString();

		return SNew(STableRow<TSharedPtr<FConversationListItem>>, OwnerTable)
			.Padding(FMargin(6, 4))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(FText::FromString(Title))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Font(FAppStyle::Get().GetFontStyle(TEXT("SmallFont")))
					.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f, 1.0f)))
					.Text(FText::FromString(Updated))
				]
			];
	}

	void OnConversationSelected(TSharedPtr<FConversationListItem> Item, ESelectInfo::Type)
	{
		if (bSuppressConversationSelectionChanged)
		{
			return;
		}
		if (!Item.IsValid())
		{
			return;
		}

		ActiveConversationId = Item->Id;
		SelectedConversationItem = Item;
		if (FConversation* C = GetActiveConversation())
		{
			Messages = C->Messages;
		}
		SyncTranscriptFromConversation();
	}

	TSharedRef<SWidget> BuildChatPane()
	{
		return SAssignNew(RightSwitcher, SWidgetSwitcher)
			.WidgetIndex(0)
			+ SWidgetSwitcher::Slot()
			[
				BuildChatMainPane()
			]
			+ SWidgetSwitcher::Slot()
			[
				BuildSettingsPane()
			];
	}

	TSharedRef<SWidget> BuildChatMainPane()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("UE Agent")))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Settings")))
					.OnClicked(this, &SUEAgentBridgePanel::OnShowSettingsClicked)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
			[
				SNew(SSeparator)
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildSelectionRow()
			]

			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 8)
			[
				SAssignNew(TranscriptBox, SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.MinDesiredHeight(80)
				[
					SAssignNew(InputBox, SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.OnKeyDownHandler(this, &SUEAgentBridgePanel::OnInputKeyDown)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Send")))
					.OnClicked(this, &SUEAgentBridgePanel::OnSendClicked)
					.IsEnabled(this, &SUEAgentBridgePanel::CanSend)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Clear")))
					.OnClicked(this, &SUEAgentBridgePanel::OnClearClicked)
				]
			];
	}

	TSharedRef<SWidget> BuildSelectionRow()
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Selection")))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("Describe")))
				.OnClicked(this, &SUEAgentBridgePanel::OnDescribeSelectionClicked)
			]
			;
	}

	FReply OnShowSettingsClicked()
	{
		if (RightSwitcher.IsValid())
		{
			RightSwitcher->SetActiveWidgetIndex(1);
		}
		return FReply::Handled();
	}

	FReply OnBackFromSettingsClicked()
	{
		if (RightSwitcher.IsValid())
		{
			RightSwitcher->SetActiveWidgetIndex(0);
		}
		return FReply::Handled();
	}

	bool CanEditBaseUrl() const
	{
		const int32 Idx = GetSelectedPresetIndex();
		return Idx != INDEX_NONE && ProviderPresets.IsValidIndex(Idx) && ProviderPresets[Idx].bAllowEditBaseUrl;
	}

	TSharedRef<SWidget> BuildSettingsPane()
	{
		return SNew(SBorder)
			.Padding(8)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("< Back")))
						.OnClicked(this, &SUEAgentBridgePanel::OnBackFromSettingsClicked)
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8, 0)
					[
						SNew(STextBlock).Text(FText::FromString(TEXT("Settings")))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(FText::FromString(TEXT("Save")))
						.OnClicked(this, &SUEAgentBridgePanel::OnSaveSettingsClicked)
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
				[
					SNew(SSeparator)
				]

				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SScrollBox)

					+ SScrollBox::Slot()
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Provider preset")))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(ProviderPresetCombo, SComboBox<TSharedPtr<FString>>)
							.OptionsSource(&ProviderPresetOptions)
							.InitiallySelectedItem(SelectedProviderPresetLabel)
							.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
							{
								return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : TEXT("")));
							})
							.OnSelectionChanged(this, &SUEAgentBridgePanel::OnProviderPresetChanged)
							[
								SNew(STextBlock).Text(this, &SUEAgentBridgePanel::GetProviderPresetText)
							]
						]

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Base URL")))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(BaseUrlBox, SEditableTextBox)
							.Text(FText::FromString(BaseUrl))
							.IsEnabled(this, &SUEAgentBridgePanel::CanEditBaseUrl)
							.OnTextCommitted(this, &SUEAgentBridgePanel::OnBaseUrlCommitted)
						]

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("API Key")))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(ApiKeyBox, SEditableTextBox)
							.IsPassword(true)
							.Text(FText::FromString(ApiKey))
							.OnTextCommitted(this, &SUEAgentBridgePanel::OnApiKeyCommitted)
						]

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Model")))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f)
							[
								SAssignNew(ModelCombo, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&ModelOptions)
								.InitiallySelectedItem(SelectedModelLabel)
								.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
								{
									return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : TEXT("")));
								})
								.OnSelectionChanged(this, &SUEAgentBridgePanel::OnModelSelected)
								[
									SNew(STextBlock).Text(this, &SUEAgentBridgePanel::GetModelText)
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("Refresh")))
								.OnClicked(this, &SUEAgentBridgePanel::OnRefreshModelsClicked)
							]
						]

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
						[
							SAssignNew(ModelBox, SEditableTextBox)
							.Text(FText::FromString(Model))
							.OnTextCommitted(this, &SUEAgentBridgePanel::OnModelCommitted)
						]

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Max steps (1-200)")))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(MaxStepsBox, SEditableTextBox)
							.Text(FText::AsNumber(MaxSteps))
						]

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 12)
						[
							SNew(SSeparator)
						]

						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
						[
							SNew(STextBlock).Text(FText::FromString(TEXT("Selection prompt")))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth()
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("Insert to chat input")))
								.OnClicked(this, &SUEAgentBridgePanel::OnInsertSelectionPromptFromSettingsClicked)
							]
						]
					]
				]
			];
	}

	FReply OnInsertSelectionPromptFromSettingsClicked()
	{
		OnInsertAIPromptClicked();
		if (RightSwitcher.IsValid())
		{
			RightSwitcher->SetActiveWidgetIndex(0);
		}
		return FReply::Handled();
	}

	void AppendTranscript(const FString& Line)
	{
		Transcript += Line + TEXT("\n");
		if (TranscriptBox.IsValid())
		{
			TranscriptBox->SetText(FText::FromString(Transcript));
			TranscriptBox->ScrollTo(ETextLocation::EndOfDocument);
		}
	}

	bool CanSend() const
	{
		return !bBusy;
	}

	FReply OnClearClicked()
	{
		Transcript.Empty();
		if (TranscriptBox.IsValid())
		{
			TranscriptBox->SetText(FText::GetEmpty());
		}
		if (FConversation* C = GetActiveConversation())
		{
			TArray<FAgentMessage> NewMsgs;
			NewMsgs.Add({TEXT("system"), GetAgentSystemPrompt()});
			C->Messages = NewMsgs;
			C->UpdatedAt = FDateTime::UtcNow();
			Messages = NewMsgs;
			RefreshConversationList();
			SaveConversations();
		}
		SyncTranscriptFromConversation();
		return FReply::Handled();
	}

	void OnBaseUrlCommitted(const FText& NewText, ETextCommit::Type)
	{
		BaseUrl = NewText.ToString();
	}
	void OnApiKeyCommitted(const FText& NewText, ETextCommit::Type)
	{
		ApiKey = NewText.ToString();
	}
	void OnModelCommitted(const FText& NewText, ETextCommit::Type)
	{
		Model = NewText.ToString();
	}

	FReply OnNewConversationClicked()
	{
		const int32 N = Conversations.Num() + 1;
		CreateNewConversation(FString::Printf(TEXT("Chat %d"), N));
		return FReply::Handled();
	}

	FReply OnDeleteConversationClicked()
	{
		DeleteActiveConversation();
		return FReply::Handled();
	}

	FReply OnDescribeSelectionClicked()
	{
		if (!Module)
		{
			AppendTranscript(TEXT("[selection] module unavailable"));
			return FReply::Handled();
		}

		int32 Code = 0;
		FString Body;
		Module->ExecuteToolForUI(TEXT("editor.get_selected_actor_details"), MakeShared<FJsonObject>(), Code, Body);
		AppendTranscript(FString::Printf(TEXT("[selection] http %d"), Code));
		AppendTranscript(Body.Left(4000));
		return FReply::Handled();
	}

	FReply OnInsertAIPromptClicked()
	{
		FString DetailsJson = TEXT("{}");
		if (Module)
		{
			int32 Code = 0;
			FString Body;
			Module->ExecuteToolForUI(TEXT("editor.get_selected_actor_details"), MakeShared<FJsonObject>(), Code, Body);
			DetailsJson = Body;
		}

		const FString Prompt =
			TEXT("You are controlling Unreal Editor through tools.\n")
			TEXT("Selected actor details (tool output):\n")
			+ DetailsJson + TEXT("\n\n")
			TEXT("Goal:\n")
			TEXT("- If the selected actor is not a Blueprint, create a Blueprint from it at /Game/AI/BP_Selected_AI and replace the actor.\n")
			TEXT("- Find suitable animation assets in /Game (idle/walk/run) using asset.search.\n")
			TEXT("- Assign an animation (or AnimBP) to the selected character using editor.set_selected_skeletal_animation.\n")
			TEXT("- Build a basic AI movement setup (wander/patrol) for the character. If you need Blueprint graph edits, use blueprint.get_graph_t3d and blueprint.paste_t3d.\n")
			TEXT("Return final instructions/results.\n");

		if (InputBox.IsValid())
		{
			InputBox->SetText(FText::FromString(Prompt));
			FSlateApplication::Get().SetKeyboardFocus(InputBox);
		}
		return FReply::Handled();
	}

	FReply OnSaveSettingsClicked()
	{
		UUEAgentBridgeSettings* Settings = GetMutableDefault<UUEAgentBridgeSettings>();
		if (BaseUrlBox.IsValid())
		{
			BaseUrl = BaseUrlBox->GetText().ToString();
		}
		if (ApiKeyBox.IsValid())
		{
			ApiKey = ApiKeyBox->GetText().ToString();
		}
		if (ModelBox.IsValid())
		{
			Model = ModelBox->GetText().ToString();
		}
		if (MaxStepsBox.IsValid())
		{
			const FString S = MaxStepsBox->GetText().ToString().TrimStartAndEnd();
			MaxSteps = FCString::Atoi(*S);
			MaxSteps = FMath::Clamp(MaxSteps, 1, 200);
			MaxStepsBox->SetText(FText::AsNumber(MaxSteps));
		}
		Settings->Provider = Provider;
		Settings->BaseUrl = BaseUrl;
		Settings->ApiKey = ApiKey;
		Settings->Model = Model;
		Settings->MaxSteps = MaxSteps;
		Settings->Temperature = Temperature;
		Settings->SaveConfig();
		AppendTranscript(TEXT("[settings] saved"));
		return FReply::Handled();
	}

	FString MakeChatUrl() const
	{
		FString Url = BaseUrl;
		Url.TrimStartAndEndInline();
		while (Url.EndsWith(TEXT("/")))
		{
			Url.LeftChopInline(1);
		}
		if (Provider == EUEAgentProvider::OllamaCloud)
		{
			return Url + TEXT("/chat");
		}
		return Url + TEXT("/chat/completions");
	}

	static FString SerializeMessagesOpenAI(const TArray<FAgentMessage>& Messages, const FString& Model, float Temperature)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("model"), Model);
		Root->SetNumberField(TEXT("temperature"), Temperature);

		TArray<TSharedPtr<FJsonValue>> MsgArr;
		for (const FAgentMessage& M : Messages)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("role"), M.Role);
			Obj->SetStringField(TEXT("content"), M.Content);
			MsgArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Root->SetArrayField(TEXT("messages"), MsgArr);

		FString Body;
		auto Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Body;
	}

	static FString SerializeMessagesOllamaCloud(const TArray<FAgentMessage>& Messages, const FString& Model, float Temperature)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("model"), Model);
		Root->SetBoolField(TEXT("stream"), false);

		TArray<TSharedPtr<FJsonValue>> MsgArr;
		for (const FAgentMessage& M : Messages)
		{
			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("role"), M.Role);
			Obj->SetStringField(TEXT("content"), M.Content);
			MsgArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Root->SetArrayField(TEXT("messages"), MsgArr);

		auto Opt = MakeShared<FJsonObject>();
		Opt->SetNumberField(TEXT("temperature"), Temperature);
		Root->SetObjectField(TEXT("options"), Opt);

		FString Body;
		auto Writer = TJsonWriterFactory<>::Create(&Body);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
		return Body;
	}

	static bool ExtractAssistantContent(const FString& ResponseBody, FString& OutContent)
	{
		TSharedPtr<FJsonObject> Root;
		if (!TryParseJsonObject(ResponseBody, Root))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
		if (!Root->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
		{
			return false;
		}

		TSharedPtr<FJsonObject> ChoiceObj = (*Choices)[0]->AsObject();
		if (!ChoiceObj.IsValid())
		{
			return false;
		}

		TSharedPtr<FJsonObject> Msg = ChoiceObj->GetObjectField(TEXT("message"));
		if (!Msg.IsValid())
		{
			return false;
		}

		return Msg->TryGetStringField(TEXT("content"), OutContent);
	}

	static bool ExtractAssistantContentOllama(const FString& ResponseBody, FString& OutContent)
	{
		TSharedPtr<FJsonObject> Root;
		if (!TryParseJsonObject(ResponseBody, Root))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Msg = nullptr;
		if (!Root->TryGetObjectField(FStringView(TEXT("message")), Msg) || !Msg || !Msg->IsValid())
		{
			return false;
		}
		return (*Msg)->TryGetStringField(TEXT("content"), OutContent);
	}

	struct FToolExecResult
	{
		int32 StatusCode = 500;
		FString BodyJson;
	};

	FToolExecResult ExecuteTool(const FString& ToolName, const TSharedPtr<FJsonObject>& InputObj)
	{
		FToolExecResult R;
		if (!Module)
		{
			R.StatusCode = 500;
			R.BodyJson = TEXT("{\"ok\":false,\"error\":\"Module unavailable\"}");
			return R;
		}

		int32 Code = 500;
		FString Body;
		if (!Module->ExecuteToolForUI(ToolName, InputObj, Code, Body))
		{
			R.StatusCode = 500;
			R.BodyJson = TEXT("{\"ok\":false,\"error\":\"ExecuteToolForUI failed\"}");
			return R;
		}

		R.StatusCode = Code;
		R.BodyJson = Body;
		return R;
	}

	void AgentStep()
	{
		if (StepsRemaining <= 0)
		{
			AppendTranscript(TEXT("[agent] max steps reached"));
			bBusy = false;
			return;
		}
		StepsRemaining--;

		const FString Url = MakeChatUrl();
		if (Url.IsEmpty() || Model.IsEmpty())
		{
			AppendTranscript(TEXT("[agent] missing BaseUrl/Model"));
			bBusy = false;
			return;
		}

		TSharedRef<IHttpRequest> Req = FHttpModule::Get().CreateRequest();
		Req->SetVerb(TEXT("POST"));
		Req->SetURL(Url);
		Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		if (!ApiKey.IsEmpty())
		{
			Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		}

		const FString Body = (Provider == EUEAgentProvider::OllamaCloud)
			? SerializeMessagesOllamaCloud(Messages, Model, Temperature)
			: SerializeMessagesOpenAI(Messages, Model, Temperature);
		Req->SetContentAsString(Body);

		const TWeakPtr<SUEAgentBridgePanel> SelfWeak = StaticCastSharedRef<SUEAgentBridgePanel>(AsShared());
		Req->OnProcessRequestComplete().BindLambda([SelfWeak](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
		{
			if (!SelfWeak.IsValid())
			{
				return;
			}
			SelfWeak.Pin()->OnLLMResponse(Resp, bOk);
		});

		ActiveRequest = Req;
		if (!Req->ProcessRequest())
		{
			ActiveRequest.Reset();
			AppendTranscript(TEXT("[llm] could not start request (ProcessRequest returned false)"));
			bBusy = false;
			return;
		}
	}

	void OnLLMResponse(FHttpResponsePtr Resp, bool bOk)
	{
		ActiveRequest.Reset();
		if (!bOk || !Resp.IsValid())
		{
			AppendTranscript(TEXT("[llm] request failed"));
			bBusy = false;
			return;
		}

		const int32 Code = Resp->GetResponseCode();
		if (Code < 200 || Code >= 300)
		{
			AppendTranscript(FString::Printf(TEXT("[llm] http %d"), Code));
			AppendTranscript(Resp->GetContentAsString().Left(4000));
			bBusy = false;
			return;
		}

		const FString RespBody = Resp->GetContentAsString();
		FString Content;
		const bool bExtracted = (Provider == EUEAgentProvider::OllamaCloud)
			? ExtractAssistantContentOllama(RespBody, Content)
			: ExtractAssistantContent(RespBody, Content);
		if (!bExtracted)
		{
			AppendTranscript(TEXT("[llm] invalid response"));
			AppendTranscript(RespBody.Left(2000));
			bBusy = false;
			return;
		}

		FString JsonText = Content;
		TSharedPtr<FJsonObject> MsgObj;
		if (!TryParseJsonObject(JsonText, MsgObj))
		{
			FString Extracted;
			if (TryExtractFirstJsonObject(Content, Extracted) && TryParseJsonObject(Extracted, MsgObj))
			{
				JsonText = Extracted;
			}
		}

		if (!MsgObj.IsValid())
		{
			// Normal text response (no JSON tool protocol).
			AppendTranscript(TEXT("[assistant] ") + Content.Left(8000));
			Messages.Add({TEXT("assistant"), Content});
			if (FConversation* C = GetActiveConversation())
			{
				C->Messages = Messages;
				C->UpdatedAt = FDateTime::UtcNow();
				RefreshConversationList();
				SaveConversations();
			}

			// If we are in an execution loop, keep going without requiring user input.
			if (StepsRemaining > 0)
			{
				AgentStep();
				return;
			}

			bBusy = false;
			return;
		}

		FString Type;
		MsgObj->TryGetStringField(TEXT("type"), Type);
		Type = Type.ToLower();

		if (Type == TEXT("final"))
		{
			FString Text;
			MsgObj->TryGetStringField(TEXT("text"), Text);
			AppendTranscript(TEXT("[assistant] ") + Text);
			Messages.Add({TEXT("assistant"), Text});
			if (FConversation* C = GetActiveConversation())
			{
				C->Messages = Messages;
				C->UpdatedAt = FDateTime::UtcNow();
				RefreshConversationList();
				SaveConversations();
			}
			bBusy = false;
			return;
		}

		if (Type == TEXT("tool_call"))
		{
			FString ToolName;
			MsgObj->TryGetStringField(TEXT("toolName"), ToolName);
			const TSharedPtr<FJsonObject>* InputPtr = nullptr;
			TSharedPtr<FJsonObject> InputObj;
			if (MsgObj->TryGetObjectField(FStringView(TEXT("input")), InputPtr) && InputPtr)
			{
				InputObj = *InputPtr;
			}

			if (ToolName.IsEmpty())
			{
				AppendTranscript(TEXT("[assistant] tool_call missing toolName"));
				bBusy = false;
				return;
			}

			AppendTranscript(TEXT("[tool] ") + ToolName);
			FToolExecResult ToolRes = ExecuteTool(ToolName, InputObj);

			// Append tool_result as next user message
			TSharedPtr<FJsonObject> ToolResultObj = MakeShared<FJsonObject>();
			ToolResultObj->SetStringField(TEXT("type"), TEXT("tool_result"));
			ToolResultObj->SetStringField(TEXT("toolName"), ToolName);
			ToolResultObj->SetNumberField(TEXT("statusCode"), ToolRes.StatusCode);

			FString CleanBody = ToolRes.BodyJson;
			{
				FString Extracted;
				if (TryExtractFirstJsonObject(CleanBody, Extracted))
				{
					CleanBody = Extracted;
				}
			}

			TSharedPtr<FJsonObject> BodyObj;
			if (TryParseJsonObject(CleanBody, BodyObj))
			{
				ToolResultObj->SetObjectField(TEXT("body"), BodyObj);
			}
			else
			{
				ToolResultObj->SetStringField(TEXT("bodyText"), CleanBody);
			}

			FString ToolResultStr;
			auto Writer = TJsonWriterFactory<>::Create(&ToolResultStr);
			FJsonSerializer::Serialize(ToolResultObj.ToSharedRef(), Writer);

			Messages.Add({TEXT("user"), ToolResultStr});
			AppendTranscript(TEXT("[tool_result] ") + ToolResultStr.Left(2000));
			if (FConversation* C = GetActiveConversation())
			{
				C->Messages = Messages;
				C->UpdatedAt = FDateTime::UtcNow();
				RefreshConversationList();
				SaveConversations();
			}

			AgentStep();
			return;
		}

		AppendTranscript(TEXT("[llm] unknown message type"));
		AppendTranscript(JsonText.Left(2000));
		bBusy = false;
	}

	FReply OnSendClicked()
	{
		SendCurrentInput();
		return FReply::Handled();
	}

private:
	void SendCurrentInput()
	{
		if (!InputBox.IsValid() || bBusy)
		{
			return;
		}

		const FString Prompt = InputBox->GetText().ToString().TrimStartAndEnd();
		if (Prompt.IsEmpty())
		{
			return;
		}

		bBusy = true;
		const int32 RequestedSteps = (MaxSteps <= 0) ? 200 : MaxSteps;
		StepsRemaining = FMath::Clamp(RequestedSteps, 1, 200);

		FConversation* C = GetActiveConversation();
		if (!C)
		{
			CreateNewConversation(TEXT("Chat"));
			C = GetActiveConversation();
		}
		if (!C)
		{
			AppendTranscript(TEXT("[chat] failed to create conversation"));
			bBusy = false;
			return;
		}

		Messages = C->Messages;
		if (Messages.Num() == 0 || Messages[0].Role != TEXT("system"))
		{
			Messages.Insert({TEXT("system"), GetAgentSystemPrompt()}, 0);
		}
		Messages.Add({TEXT("user"), Prompt});
		C->Messages = Messages;
		C->UpdatedAt = FDateTime::UtcNow();
		RefreshConversationList();
		SaveConversations();

		MaybeAutoTitleActiveConversation(Prompt);

		AppendTranscript(TEXT("[user] ") + Prompt);
		InputBox->SetText(FText::GetEmpty());

		AgentStep();
	}

	FReply OnInputKeyDown(const FGeometry&, const FKeyEvent& InKeyEvent)
	{
		if (InKeyEvent.GetKey() == EKeys::Enter)
		{
			// Shift+Enter: newline
			if (InKeyEvent.IsShiftDown())
			{
				return FReply::Unhandled();
			}

			// Enter: send
			SendCurrentInput();
			return FReply::Handled();
		}

		return FReply::Unhandled();
	}

	class FUEAgentBridgeModule* Module = nullptr;

	TSharedPtr<SMultiLineEditableTextBox> TranscriptBox;
	TSharedPtr<SMultiLineEditableTextBox> InputBox;
	TSharedPtr<SEditableTextBox> BaseUrlBox;
	TSharedPtr<SEditableTextBox> ApiKeyBox;
	TSharedPtr<SEditableTextBox> ModelBox;
	TSharedPtr<SEditableTextBox> MaxStepsBox;
	TSharedPtr<SWidgetSwitcher> RightSwitcher;

	TArray<FProviderPreset> ProviderPresets;
	TArray<TSharedPtr<FString>> ProviderPresetOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ProviderPresetCombo;
	TSharedPtr<FString> SelectedProviderPresetLabel;

	TArray<TSharedPtr<FString>> ModelOptions;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelCombo;
	TSharedPtr<FString> SelectedModelLabel;

	FString Transcript;

	EUEAgentProvider Provider = EUEAgentProvider::OpenAICompatible;
	FString BaseUrl;
	FString ApiKey;
	FString Model;
	int32 MaxSteps = 8;
	float Temperature = 0.0f;

	bool bBusy = false;
	int32 StepsRemaining = 0;
	TArray<FAgentMessage> Messages;

	FHttpRequestPtr ActiveRequest;

	TArray<FConversation> Conversations;
	FString ActiveConversationId;
	TArray<TSharedPtr<FConversationListItem>> ConversationListItems;
	TSharedPtr<SListView<TSharedPtr<FConversationListItem>>> ConversationListView;
	TSharedPtr<FConversationListItem> SelectedConversationItem;
	bool bSuppressConversationSelectionChanged = false;
};

static TUniquePtr<FHttpServerResponse> JsonResponse(const TSharedPtr<FJsonObject>& Obj, int32 Code = 200)
{
	FString Body;
	auto Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);

	auto Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
	Response->Code = static_cast<EHttpServerResponseCodes>(Code);
	return Response;
}

static bool TryGetFirstHeaderValue(const FHttpServerRequest& Request, const FString& HeaderName, FString& OutValue)
{
	for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
	{
		if (!Pair.Key.Equals(HeaderName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Pair.Value.Num() > 0)
		{
			OutValue = Pair.Value[0];
			return true;
		}
		return false;
	}
	return false;
}

static bool CheckAuth(const FHttpServerRequest& Request, const FString& Token)
{
	if (Token.IsEmpty())
	{
		return true;
	}

	FString HeaderValue;
	if (!TryGetFirstHeaderValue(Request, TEXT("authorization"), HeaderValue))
	{
		return false;
	}

	HeaderValue.TrimStartAndEndInline();
	if (HeaderValue.StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase))
	{
		HeaderValue.RightChopInline(7);
		HeaderValue.TrimStartAndEndInline();
	}

	return HeaderValue == Token;
}

void FUEAgentBridgeModule::StartupModule()
{
	const FString PortEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_AGENT_BRIDGE_PORT"));
	if (!PortEnv.IsEmpty())
	{
		Port = (uint16)FCString::Atoi(*PortEnv);
	}

	Token = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_AGENT_BRIDGE_TOKEN"));
	StartServer();

	if (!IsRunningCommandlet())
	{
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			UEAgentBridgeTabName,
			FOnSpawnTab::CreateLambda([this](const FSpawnTabArgs&)
			{
				return SNew(SDockTab)
					.TabRole(ETabRole::NomadTab)
					.Label(FText::FromString(TEXT("UE Agent")))
					[
						SNew(SUEAgentBridgePanel, this)
					];
			}))
			.SetDisplayName(FText::FromString(TEXT("UE Agent")))
			.SetMenuType(ETabSpawnerMenuType::Hidden);

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateLambda([Owner = this]()
			{
				UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window"));
				if (!Menu)
				{
					return;
				}

				FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("WindowLayout"));
				Section.AddMenuEntry(
					TEXT("UEAgentBridge.OpenTab"),
					FText::FromString(TEXT("UE Agent")),
					FText::FromString(TEXT("Open the UE Agent panel.")),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([]()
					{
						FGlobalTabmanager::Get()->TryInvokeTab(UEAgentBridgeTabName);
					}))
				);
			})
		);
	}
}

void FUEAgentBridgeModule::ShutdownModule()
{
	if (!IsRunningCommandlet())
	{
		UToolMenus::UnregisterOwner(this);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UEAgentBridgeTabName);
	}
	StopServer();
}

void FUEAgentBridgeModule::StartServer()
{
	if (Router.IsValid())
	{
		return;
	}

	FHttpServerModule& HttpServer = FHttpServerModule::Get();
	Router = HttpServer.GetHttpRouter(Port);
	RegisterRoutes();
	HttpServer.StartAllListeners();
}

void FUEAgentBridgeModule::StopServer()
{
	if (!Router.IsValid())
	{
		return;
	}

	FHttpServerModule::Get().StopAllListeners();
	Router.Reset();
}

void FUEAgentBridgeModule::RegisterRoutes()
{
	check(Router.IsValid());

	Router->BindRoute(
		FHttpPath(TEXT("/ue-agent/health")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleHealth(Request, OnComplete);
		}));

	Router->BindRoute(
		FHttpPath(TEXT("/ue-agent/tools")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleTools(Request, OnComplete);
		}));

	Router->BindRoute(
		FHttpPath(TEXT("/ue-agent/tools/call")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleToolCall(Request, OnComplete);
		}));
}

bool FUEAgentBridgeModule::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, Token))
	{
		OnComplete(JsonResponse(MakeShared<FJsonObject>(), 401));
		return true;
	}

	auto Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("ok"), true);
	Obj->SetNumberField(TEXT("port"), Port);
	Obj->SetStringField(TEXT("projectDir"), FPaths::ProjectDir());
	OnComplete(JsonResponse(Obj, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTools(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, Token))
	{
		OnComplete(JsonResponse(MakeShared<FJsonObject>(), 401));
		return true;
	}

	auto Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Tools;

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("project.get_directory"));
		Tool->SetStringField(TEXT("description"), TEXT("Return the current Unreal project directory."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("editor.get_selected_actors"));
		Tool->SetStringField(TEXT("description"), TEXT("List selected actor names in the editor."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("editor.get_selected_actor_details"));
		Tool->SetStringField(TEXT("description"), TEXT("Get details for selected actors (class, path, skeletal mesh info)."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("editor.create_blueprint_from_selected_actor"));
		Tool->SetStringField(TEXT("description"), TEXT("Create a Blueprint from the single currently selected actor. Optionally replace the actor in the level."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto AssetPathProp = MakeShared<FJsonObject>();
		AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
		AssetPathProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("assetPath"), AssetPathProp);

		auto ReplaceProp = MakeShared<FJsonObject>();
		ReplaceProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("replaceActor"), ReplaceProp);

		auto OpenProp = MakeShared<FJsonObject>();
		OpenProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("openBlueprint"), OpenProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("assetPath")));
		Schema->SetArrayField(TEXT("required"), ReqArr);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("editor.set_selected_skeletal_animation"));
		Tool->SetStringField(TEXT("description"), TEXT("Set animation on the first SkeletalMeshComponent of the single selected actor (AnimBP class or animation asset)."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto AnimBPProp = MakeShared<FJsonObject>();
		AnimBPProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("animBlueprintClassPath"), AnimBPProp);

		auto AnimAssetProp = MakeShared<FJsonObject>();
		AnimAssetProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("animationAssetPath"), AnimAssetProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("project.get_name"));
		Tool->SetStringField(TEXT("description"), TEXT("Return the project name."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("editor.get_world_info"));
		Tool->SetStringField(TEXT("description"), TEXT("Return current editor world info (map name, actor count)."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("level.save_current"));
		Tool->SetStringField(TEXT("description"), TEXT("Save the currently opened level."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("level.spawn_actor"));
		Tool->SetStringField(TEXT("description"), TEXT("Spawn an actor in the current editor world by class path."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto Number = []()
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("type"), TEXT("number"));
			return O;
		};

		auto Location = MakeShared<FJsonObject>();
		Location->SetStringField(TEXT("type"), TEXT("object"));
		Location->SetBoolField(TEXT("additionalProperties"), false);
		auto LocProps = MakeShared<FJsonObject>();
		LocProps->SetObjectField(TEXT("x"), Number());
		LocProps->SetObjectField(TEXT("y"), Number());
		LocProps->SetObjectField(TEXT("z"), Number());
		Location->SetObjectField(TEXT("properties"), LocProps);

		auto Rotation = MakeShared<FJsonObject>();
		Rotation->SetStringField(TEXT("type"), TEXT("object"));
		Rotation->SetBoolField(TEXT("additionalProperties"), false);
		auto RotProps = MakeShared<FJsonObject>();
		RotProps->SetObjectField(TEXT("pitch"), Number());
		RotProps->SetObjectField(TEXT("yaw"), Number());
		RotProps->SetObjectField(TEXT("roll"), Number());
		Rotation->SetObjectField(TEXT("properties"), RotProps);

		auto Scale = MakeShared<FJsonObject>();
		Scale->SetStringField(TEXT("type"), TEXT("object"));
		Scale->SetBoolField(TEXT("additionalProperties"), false);
		auto ScaleProps = MakeShared<FJsonObject>();
		ScaleProps->SetObjectField(TEXT("x"), Number());
		ScaleProps->SetObjectField(TEXT("y"), Number());
		ScaleProps->SetObjectField(TEXT("z"), Number());
		Scale->SetObjectField(TEXT("properties"), ScaleProps);

		auto ClassPath = MakeShared<FJsonObject>();
		ClassPath->SetStringField(TEXT("type"), TEXT("string"));
		ClassPath->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("classPath"), ClassPath);

		auto NameProp = MakeShared<FJsonObject>();
		NameProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("name"), NameProp);

		Props->SetObjectField(TEXT("location"), Location);
		Props->SetObjectField(TEXT("rotation"), Rotation);
		Props->SetObjectField(TEXT("scale"), Scale);
		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("classPath")));
		Schema->SetArrayField(TEXT("required"), ReqArr);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("asset.create_blueprint"));
		Tool->SetStringField(TEXT("description"), TEXT("Create a Blueprint asset at /Game/... with a given parent class (e.g. /Script/Engine.Actor)."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();
		auto AssetPathProp = MakeShared<FJsonObject>();
		AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
		AssetPathProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("assetPath"), AssetPathProp);

		auto ParentProp = MakeShared<FJsonObject>();
		ParentProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("parentClassPath"), ParentProp);
		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("assetPath")));
		Schema->SetArrayField(TEXT("required"), ReqArr);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("asset.search"));
		Tool->SetStringField(TEXT("description"), TEXT("Search assets by package path (e.g. /Game) and optional class paths."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto PackageProp = MakeShared<FJsonObject>();
		PackageProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("packagePath"), PackageProp);

		auto ClassPathsProp = MakeShared<FJsonObject>();
		ClassPathsProp->SetStringField(TEXT("type"), TEXT("array"));
		auto ClassItem = MakeShared<FJsonObject>();
		ClassItem->SetStringField(TEXT("type"), TEXT("string"));
		ClassPathsProp->SetObjectField(TEXT("items"), ClassItem);
		Props->SetObjectField(TEXT("classPaths"), ClassPathsProp);

		auto LimitProp = MakeShared<FJsonObject>();
		LimitProp->SetStringField(TEXT("type"), TEXT("number"));
		Props->SetObjectField(TEXT("limit"), LimitProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("blueprint.get_graph_t3d"));
		Tool->SetStringField(TEXT("description"), TEXT("Export all nodes from a Blueprint graph as T3D text (copy/paste format)."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BpProp = MakeShared<FJsonObject>();
		BpProp->SetStringField(TEXT("type"), TEXT("string"));
		BpProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BpProp);

		auto GraphNameProp = MakeShared<FJsonObject>();
		GraphNameProp->SetStringField(TEXT("type"), TEXT("string"));
		GraphNameProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("graphName"), GraphNameProp);

		auto GraphTypeProp = MakeShared<FJsonObject>();
		GraphTypeProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("graphType"), GraphTypeProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("graphName")));
		Schema->SetArrayField(TEXT("required"), ReqArr);

		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("blueprint.paste_t3d"));
		Tool->SetStringField(TEXT("description"), TEXT("Import/paste T3D nodes into a Blueprint graph. Optionally clear existing nodes."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BpProp = MakeShared<FJsonObject>();
		BpProp->SetStringField(TEXT("type"), TEXT("string"));
		BpProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BpProp);

		auto GraphNameProp = MakeShared<FJsonObject>();
		GraphNameProp->SetStringField(TEXT("type"), TEXT("string"));
		GraphNameProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("graphName"), GraphNameProp);

		auto GraphTypeProp = MakeShared<FJsonObject>();
		GraphTypeProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("graphType"), GraphTypeProp);

		auto T3DProp = MakeShared<FJsonObject>();
		T3DProp->SetStringField(TEXT("type"), TEXT("string"));
		T3DProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("t3d"), T3DProp);

		auto ClearProp = MakeShared<FJsonObject>();
		ClearProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("clearExisting"), ClearProp);

		auto CompileProp = MakeShared<FJsonObject>();
		CompileProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("compile"), CompileProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("graphName")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("t3d")));
		Schema->SetArrayField(TEXT("required"), ReqArr);

		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.create_widget_blueprint"));
		Tool->SetStringField(TEXT("description"), TEXT("Create a WidgetBlueprint asset at /Game/... (UMG)."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto AssetPathProp = MakeShared<FJsonObject>();
		AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
		AssetPathProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("assetPath"), AssetPathProp);

		auto ParentProp = MakeShared<FJsonObject>();
		ParentProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("parentClassPath"), ParentProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("assetPath")));
		Schema->SetArrayField(TEXT("required"), ReqArr);

		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.add_widget"));
		Tool->SetStringField(TEXT("description"), TEXT("Add a widget to a WidgetBlueprint's WidgetTree."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);

		auto ClassProp = MakeShared<FJsonObject>();
		ClassProp->SetStringField(TEXT("type"), TEXT("string"));
		ClassProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("widgetClassPath"), ClassProp);

		auto NameProp = MakeShared<FJsonObject>();
		NameProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("name"), NameProp);

		auto ParentNameProp = MakeShared<FJsonObject>();
		ParentNameProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("parentName"), ParentNameProp);

		auto WrapProp = MakeShared<FJsonObject>();
		WrapProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("wrapRootInCanvas"), WrapProp);

		auto Layout = MakeShared<FJsonObject>();
		Layout->SetStringField(TEXT("type"), TEXT("object"));
		Layout->SetBoolField(TEXT("additionalProperties"), false);
		auto LayoutProps = MakeShared<FJsonObject>();

		auto Number = []()
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("type"), TEXT("number"));
			return O;
		};

		auto Vec2 = MakeShared<FJsonObject>();
		Vec2->SetStringField(TEXT("type"), TEXT("object"));
		Vec2->SetBoolField(TEXT("additionalProperties"), false);
		auto Vec2Props = MakeShared<FJsonObject>();
		Vec2Props->SetObjectField(TEXT("x"), Number());
		Vec2Props->SetObjectField(TEXT("y"), Number());
		Vec2->SetObjectField(TEXT("properties"), Vec2Props);

		LayoutProps->SetObjectField(TEXT("position"), Vec2);
		LayoutProps->SetObjectField(TEXT("size"), Vec2);
		LayoutProps->SetObjectField(TEXT("anchorMin"), Vec2);
		LayoutProps->SetObjectField(TEXT("anchorMax"), Vec2);
		LayoutProps->SetObjectField(TEXT("alignment"), Vec2);
		LayoutProps->SetObjectField(TEXT("zOrder"), Number());
		Layout->SetObjectField(TEXT("properties"), LayoutProps);

		Props->SetObjectField(TEXT("canvasLayout"), Layout);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("widgetClassPath")));
		Schema->SetArrayField(TEXT("required"), ReqArr);

		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.set_text"));
		Tool->SetStringField(TEXT("description"), TEXT("Set text on a TextBlock/RichTextBlock by widget name."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);

		auto NameProp = MakeShared<FJsonObject>();
		NameProp->SetStringField(TEXT("type"), TEXT("string"));
		NameProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("widgetName"), NameProp);

		auto TextProp = MakeShared<FJsonObject>();
		TextProp->SetStringField(TEXT("type"), TEXT("string"));
		Props->SetObjectField(TEXT("text"), TextProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("widgetName")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("text")));
		Schema->SetArrayField(TEXT("required"), ReqArr);

		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.set_properties"));
		Tool->SetStringField(TEXT("description"), TEXT("Set a safe allowlist of widget/slot properties on a WidgetBlueprint widget."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);

		auto NameProp = MakeShared<FJsonObject>();
		NameProp->SetStringField(TEXT("type"), TEXT("string"));
		NameProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("widgetName"), NameProp);

		auto BoolProp = []()
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("type"), TEXT("boolean"));
			return O;
		};
		auto NumProp = []()
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("type"), TEXT("number"));
			return O;
		};
		auto ColorProp = []()
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("type"), TEXT("object"));
			O->SetBoolField(TEXT("additionalProperties"), false);
			auto P = MakeShared<FJsonObject>();
			auto N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("type"), TEXT("number"));
			P->SetObjectField(TEXT("r"), N);
			P->SetObjectField(TEXT("g"), N);
			P->SetObjectField(TEXT("b"), N);
			P->SetObjectField(TEXT("a"), N);
			O->SetObjectField(TEXT("properties"), P);
			return O;
		};

		auto Layout = MakeShared<FJsonObject>();
		Layout->SetStringField(TEXT("type"), TEXT("object"));
		Layout->SetBoolField(TEXT("additionalProperties"), false);
		auto LayoutProps = MakeShared<FJsonObject>();
		auto Vec2 = []()
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("type"), TEXT("object"));
			O->SetBoolField(TEXT("additionalProperties"), false);
			auto P = MakeShared<FJsonObject>();
			auto N = MakeShared<FJsonObject>();
			N->SetStringField(TEXT("type"), TEXT("number"));
			P->SetObjectField(TEXT("x"), N);
			P->SetObjectField(TEXT("y"), N);
			O->SetObjectField(TEXT("properties"), P);
			return O;
		};
		LayoutProps->SetObjectField(TEXT("position"), Vec2());
		LayoutProps->SetObjectField(TEXT("size"), Vec2());
		LayoutProps->SetObjectField(TEXT("anchorMin"), Vec2());
		LayoutProps->SetObjectField(TEXT("anchorMax"), Vec2());
		LayoutProps->SetObjectField(TEXT("alignment"), Vec2());
		LayoutProps->SetObjectField(TEXT("zOrder"), NumProp());
		Layout->SetObjectField(TEXT("properties"), LayoutProps);

		auto PropBag = MakeShared<FJsonObject>();
		PropBag->SetStringField(TEXT("type"), TEXT("object"));
		PropBag->SetBoolField(TEXT("additionalProperties"), false);
		auto BagProps = MakeShared<FJsonObject>();
		auto StrProp = []()
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("type"), TEXT("string"));
			return O;
		};
		BagProps->SetObjectField(TEXT("text"), StrProp());
		BagProps->SetObjectField(TEXT("visibility"), StrProp());
		BagProps->SetObjectField(TEXT("isEnabled"), BoolProp());
		BagProps->SetObjectField(TEXT("renderOpacity"), NumProp());
		BagProps->SetObjectField(TEXT("colorAndOpacity"), ColorProp());
		BagProps->SetObjectField(TEXT("fontSize"), NumProp());
		BagProps->SetObjectField(TEXT("tintColor"), ColorProp());
		BagProps->SetObjectField(TEXT("texturePath"), StrProp());
		BagProps->SetObjectField(TEXT("canvasLayout"), Layout);
		PropBag->SetObjectField(TEXT("properties"), BagProps);

		Props->SetObjectField(TEXT("properties"), PropBag);

		auto CompileProp = MakeShared<FJsonObject>();
		CompileProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("compile"), CompileProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("widgetName")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("properties")));
		Schema->SetArrayField(TEXT("required"), ReqArr);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.scaffold_layout"));
		Tool->SetStringField(TEXT("description"), TEXT("Scaffold a full WidgetTree layout from an ordered list of elements."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);

		auto Elem = MakeShared<FJsonObject>();
		Elem->SetStringField(TEXT("type"), TEXT("object"));
		Elem->SetBoolField(TEXT("additionalProperties"), false);
		auto ElemProps = MakeShared<FJsonObject>();

		auto ClassProp = MakeShared<FJsonObject>();
		ClassProp->SetStringField(TEXT("type"), TEXT("string"));
		ClassProp->SetNumberField(TEXT("minLength"), 1);
		ElemProps->SetObjectField(TEXT("widgetClassPath"), ClassProp);

		auto NameProp = MakeShared<FJsonObject>();
		NameProp->SetStringField(TEXT("type"), TEXT("string"));
		ElemProps->SetObjectField(TEXT("name"), NameProp);

		auto ParentProp = MakeShared<FJsonObject>();
		ParentProp->SetStringField(TEXT("type"), TEXT("string"));
		ElemProps->SetObjectField(TEXT("parentName"), ParentProp);

		{
			auto NumProp = []()
			{
				auto O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("type"), TEXT("number"));
				return O;
			};
			auto Vec2 = []()
			{
				auto O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("type"), TEXT("object"));
				O->SetBoolField(TEXT("additionalProperties"), false);
				auto P = MakeShared<FJsonObject>();
				auto N = MakeShared<FJsonObject>();
				N->SetStringField(TEXT("type"), TEXT("number"));
				P->SetObjectField(TEXT("x"), N);
				P->SetObjectField(TEXT("y"), N);
				O->SetObjectField(TEXT("properties"), P);
				return O;
			};

			auto Layout = MakeShared<FJsonObject>();
			Layout->SetStringField(TEXT("type"), TEXT("object"));
			Layout->SetBoolField(TEXT("additionalProperties"), false);
			auto LayoutProps = MakeShared<FJsonObject>();
			LayoutProps->SetObjectField(TEXT("position"), Vec2());
			LayoutProps->SetObjectField(TEXT("size"), Vec2());
			LayoutProps->SetObjectField(TEXT("anchorMin"), Vec2());
			LayoutProps->SetObjectField(TEXT("anchorMax"), Vec2());
			LayoutProps->SetObjectField(TEXT("alignment"), Vec2());
			LayoutProps->SetObjectField(TEXT("zOrder"), NumProp());
			Layout->SetObjectField(TEXT("properties"), LayoutProps);
			ElemProps->SetObjectField(TEXT("canvasLayout"), Layout);
		}

		auto PropsBag = MakeShared<FJsonObject>();
		PropsBag->SetStringField(TEXT("type"), TEXT("object"));
		PropsBag->SetBoolField(TEXT("additionalProperties"), false);
		{
			auto BoolProp = []()
			{
				auto O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("type"), TEXT("boolean"));
				return O;
			};
			auto NumProp = []()
			{
				auto O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("type"), TEXT("number"));
				return O;
			};
			auto StrProp = []()
			{
				auto O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("type"), TEXT("string"));
				return O;
			};
			auto ColorProp = []()
			{
				auto O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("type"), TEXT("object"));
				O->SetBoolField(TEXT("additionalProperties"), false);
				auto P = MakeShared<FJsonObject>();
				auto N = MakeShared<FJsonObject>();
				N->SetStringField(TEXT("type"), TEXT("number"));
				P->SetObjectField(TEXT("r"), N);
				P->SetObjectField(TEXT("g"), N);
				P->SetObjectField(TEXT("b"), N);
				P->SetObjectField(TEXT("a"), N);
				O->SetObjectField(TEXT("properties"), P);
				return O;
			};

			auto BagProps = MakeShared<FJsonObject>();
			BagProps->SetObjectField(TEXT("text"), StrProp());
			BagProps->SetObjectField(TEXT("visibility"), StrProp());
			BagProps->SetObjectField(TEXT("isEnabled"), BoolProp());
			BagProps->SetObjectField(TEXT("renderOpacity"), NumProp());
			BagProps->SetObjectField(TEXT("colorAndOpacity"), ColorProp());
			BagProps->SetObjectField(TEXT("fontSize"), NumProp());
			BagProps->SetObjectField(TEXT("tintColor"), ColorProp());
			BagProps->SetObjectField(TEXT("texturePath"), StrProp());

			PropsBag->SetObjectField(TEXT("properties"), BagProps);
		}
		ElemProps->SetObjectField(TEXT("properties"), PropsBag);

		Elem->SetObjectField(TEXT("properties"), ElemProps);
		TArray<TSharedPtr<FJsonValue>> ElemReq;
		ElemReq.Add(MakeShared<FJsonValueString>(TEXT("widgetClassPath")));
		Elem->SetArrayField(TEXT("required"), ElemReq);

		auto ElementsProp = MakeShared<FJsonObject>();
		ElementsProp->SetStringField(TEXT("type"), TEXT("array"));
		ElementsProp->SetObjectField(TEXT("items"), Elem);
		Props->SetObjectField(TEXT("elements"), ElementsProp);

		auto CompileProp = MakeShared<FJsonObject>();
		CompileProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("compile"), CompileProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("elements")));
		Schema->SetArrayField(TEXT("required"), ReqArr);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.bind_event"));
		Tool->SetStringField(TEXT("description"), TEXT("Bind a widget event delegate (e.g., Button.OnClicked) to a handler function on the WidgetBlueprint."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);

		auto WidgetProp = MakeShared<FJsonObject>();
		WidgetProp->SetStringField(TEXT("type"), TEXT("string"));
		WidgetProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("widgetName"), WidgetProp);

		auto EventProp = MakeShared<FJsonObject>();
		EventProp->SetStringField(TEXT("type"), TEXT("string"));
		EventProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("eventName"), EventProp);

		auto HandlerProp = MakeShared<FJsonObject>();
		HandlerProp->SetStringField(TEXT("type"), TEXT("string"));
		HandlerProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("handlerFunctionName"), HandlerProp);

		auto CreateProp = MakeShared<FJsonObject>();
		CreateProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("createHandlerIfMissing"), CreateProp);

		auto CompileProp = MakeShared<FJsonObject>();
		CompileProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("compile"), CompileProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("widgetName")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("eventName")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("handlerFunctionName")));
		Schema->SetArrayField(TEXT("required"), ReqArr);

		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.bind_property"));
		Tool->SetStringField(TEXT("description"), TEXT("Bind a widget property (e.g., TextBlock.Text) to a handler function (pure, matching signature)."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);

		auto WidgetProp = MakeShared<FJsonObject>();
		WidgetProp->SetStringField(TEXT("type"), TEXT("string"));
		WidgetProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("widgetName"), WidgetProp);

		auto PropName = MakeShared<FJsonObject>();
		PropName->SetStringField(TEXT("type"), TEXT("string"));
		PropName->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("propertyName"), PropName);

		auto HandlerProp = MakeShared<FJsonObject>();
		HandlerProp->SetStringField(TEXT("type"), TEXT("string"));
		HandlerProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("handlerFunctionName"), HandlerProp);

		auto CreateProp = MakeShared<FJsonObject>();
		CreateProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("createHandlerIfMissing"), CreateProp);

		auto CompileProp = MakeShared<FJsonObject>();
		CompileProp->SetStringField(TEXT("type"), TEXT("boolean"));
		Props->SetObjectField(TEXT("compile"), CompileProp);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("widgetName")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("propertyName")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("handlerFunctionName")));
		Schema->SetArrayField(TEXT("required"), ReqArr);

		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.unbind"));
		Tool->SetStringField(TEXT("description"), TEXT("Remove a WidgetBlueprint binding (property binding or event binding) from a widget."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();

		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);

		auto WidgetProp = MakeShared<FJsonObject>();
		WidgetProp->SetStringField(TEXT("type"), TEXT("string"));
		WidgetProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("widgetName"), WidgetProp);

		auto PropName = MakeShared<FJsonObject>();
		PropName->SetStringField(TEXT("type"), TEXT("string"));
		PropName->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("propertyName"), PropName);

		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("widgetName")));
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("propertyName")));
		Schema->SetArrayField(TEXT("required"), ReqArr);

		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.list_widgets"));
		Tool->SetStringField(TEXT("description"), TEXT("List widgets (name + class) from a WidgetBlueprint WidgetTree."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();
		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);
		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		Schema->SetArrayField(TEXT("required"), ReqArr);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	{
		auto Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), TEXT("umg.compile"));
		Tool->SetStringField(TEXT("description"), TEXT("Compile a WidgetBlueprint."));
		auto Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		auto Props = MakeShared<FJsonObject>();
		auto BPProp = MakeShared<FJsonObject>();
		BPProp->SetStringField(TEXT("type"), TEXT("string"));
		BPProp->SetNumberField(TEXT("minLength"), 1);
		Props->SetObjectField(TEXT("blueprintPath"), BPProp);
		Schema->SetObjectField(TEXT("properties"), Props);
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		ReqArr.Add(MakeShared<FJsonValueString>(TEXT("blueprintPath")));
		Schema->SetArrayField(TEXT("required"), ReqArr);
		Tool->SetObjectField(TEXT("inputSchema"), Schema);
		Tools.Add(MakeShared<FJsonValueObject>(Tool));
	}

	Root->SetArrayField(TEXT("tools"), Tools);
	OnComplete(JsonResponse(Root, 200));
	return true;
}

static bool ParseJsonBody(const FHttpServerRequest& Request, TSharedPtr<FJsonObject>& OutObj)
{
	if (Request.Body.Num() <= 0)
	{
		return false;
	}

	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Request.Body.GetData());
	FUTF8ToTCHAR Convert(Data, Request.Body.Num());
	const FString Body(Convert.Get(), Convert.Length());
	if (Body.IsEmpty())
	{
		return false;
	}

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	return FJsonSerializer::Deserialize(Reader, OutObj) && OutObj.IsValid();
}

bool FUEAgentBridgeModule::ExecuteToolForUI(const FString& ToolName, const TSharedPtr<FJsonObject>& Input, int32& OutStatusCode, FString& OutBodyJson)
{
	OutStatusCode = 500;
	OutBodyJson = TEXT("{\"ok\":false,\"error\":\"Unhandled\"}");

	const FString ToolKey = ToolName.ToLower();

	TUniquePtr<FHttpServerResponse> Captured;
	FHttpResultCallback Cb = [&Captured](TUniquePtr<FHttpServerResponse>&& Resp)
	{
		Captured = MoveTemp(Resp);
	};

	if (ToolKey == TEXT("project.get_directory")) { HandleTool_ProjectGetDirectory(Cb); }
	else if (ToolKey == TEXT("project.get_name")) { HandleTool_ProjectGetName(Cb); }
	else if (ToolKey == TEXT("editor.get_selected_actors")) { HandleTool_EditorGetSelectedActors(Cb); }
	else if (ToolKey == TEXT("editor.get_world_info")) { HandleTool_EditorGetWorldInfo(Cb); }
	else if (ToolKey == TEXT("editor.get_selected_actor_details")) { HandleTool_EditorGetSelectedActorDetails(Cb); }
	else if (ToolKey == TEXT("editor.create_blueprint_from_selected_actor")) { HandleTool_EditorCreateBlueprintFromSelectedActor(Input, Cb); }
	else if (ToolKey == TEXT("editor.set_selected_skeletal_animation")) { HandleTool_EditorSetSelectedSkeletalAnimation(Input, Cb); }
	else if (ToolKey == TEXT("level.save_current")) { HandleTool_LevelSaveCurrent(Cb); }
	else if (ToolKey == TEXT("level.spawn_actor")) { HandleTool_LevelSpawnActor(Input, Cb); }
	else if (ToolKey == TEXT("asset.search")) { HandleTool_AssetSearch(Input, Cb); }
	else if (ToolKey == TEXT("asset.create_blueprint")) { HandleTool_AssetCreateBlueprint(Input, Cb); }
	else if (ToolKey == TEXT("blueprint.get_graph_t3d")) { HandleTool_BlueprintGetGraphT3D(Input, Cb); }
	else if (ToolKey == TEXT("blueprint.paste_t3d")) { HandleTool_BlueprintPasteT3D(Input, Cb); }
	else if (ToolKey == TEXT("umg.create_widget_blueprint")) { HandleTool_UmgCreateWidgetBlueprint(Input, Cb); }
	else if (ToolKey == TEXT("umg.add_widget")) { HandleTool_UmgAddWidget(Input, Cb); }
	else if (ToolKey == TEXT("umg.set_text")) { HandleTool_UmgSetText(Input, Cb); }
	else if (ToolKey == TEXT("umg.set_properties")) { HandleTool_UmgSetProperties(Input, Cb); }
	else if (ToolKey == TEXT("umg.scaffold_layout")) { HandleTool_UmgScaffoldLayout(Input, Cb); }
	else if (ToolKey == TEXT("umg.bind_event")) { HandleTool_UmgBindEvent(Input, Cb); }
	else if (ToolKey == TEXT("umg.bind_property")) { HandleTool_UmgBindProperty(Input, Cb); }
	else if (ToolKey == TEXT("umg.unbind")) { HandleTool_UmgUnbind(Input, Cb); }
	else if (ToolKey == TEXT("umg.list_widgets")) { HandleTool_UmgListWidgets(Input, Cb); }
	else if (ToolKey == TEXT("umg.compile")) { HandleTool_UmgCompile(Input, Cb); }
	else
	{
		auto Err = MakeShared<FJsonObject>();
		Err->SetBoolField(TEXT("ok"), false);
		Err->SetStringField(TEXT("error"), TEXT("Unknown tool"));
		Err->SetStringField(TEXT("toolName"), ToolName);
		Captured = JsonResponse(Err, 404);
	}

	if (!Captured)
	{
		OutStatusCode = 500;
		OutBodyJson = TEXT("{\"ok\":false,\"error\":\"No response\"}");
		return false;
	}

	OutStatusCode = (int32)Captured->Code;
	OutBodyJson = HttpResponseBodyToString(*Captured);
	{
		FString Extracted;
		if (TryExtractFirstJsonObject(OutBodyJson, Extracted))
		{
			OutBodyJson = Extracted;
		}
	}
	return true;
}

bool FUEAgentBridgeModule::HandleToolCall(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (!CheckAuth(Request, Token))
	{
		OnComplete(JsonResponse(MakeShared<FJsonObject>(), 401));
		return true;
	}

	TSharedPtr<FJsonObject> BodyObj;
	if (!ParseJsonBody(Request, BodyObj))
	{
		auto Err = MakeShared<FJsonObject>();
		Err->SetBoolField(TEXT("ok"), false);
		Err->SetStringField(TEXT("error"), TEXT("Invalid JSON body"));
		OnComplete(JsonResponse(Err, 400));
		return true;
	}

	FString ToolName;
	if (!BodyObj->TryGetStringField(TEXT("toolName"), ToolName))
	{
		auto Err = MakeShared<FJsonObject>();
		Err->SetBoolField(TEXT("ok"), false);
		Err->SetStringField(TEXT("error"), TEXT("Missing toolName"));
		OnComplete(JsonResponse(Err, 400));
		return true;
	}

	TSharedPtr<FJsonObject> InputObj;
	{
		const TSharedPtr<FJsonObject>* InputPtr = nullptr;
		if (BodyObj->TryGetObjectField(FStringView(TEXT("input")), InputPtr) && InputPtr)
		{
			InputObj = *InputPtr;
		}
	}

	AsyncTask(ENamedThreads::GameThread, [this, ToolName, InputObj, OnComplete]()
	{
		if (ToolName == TEXT("project.get_directory"))
		{
			HandleTool_ProjectGetDirectory(OnComplete);
			return;
		}
		if (ToolName == TEXT("project.get_name"))
		{
			HandleTool_ProjectGetName(OnComplete);
			return;
		}
		if (ToolName == TEXT("editor.get_selected_actors"))
		{
			HandleTool_EditorGetSelectedActors(OnComplete);
			return;
		}
		if (ToolName == TEXT("editor.get_selected_actor_details"))
		{
			HandleTool_EditorGetSelectedActorDetails(OnComplete);
			return;
		}
		if (ToolName == TEXT("editor.get_world_info"))
		{
			HandleTool_EditorGetWorldInfo(OnComplete);
			return;
		}
		if (ToolName == TEXT("editor.create_blueprint_from_selected_actor"))
		{
			HandleTool_EditorCreateBlueprintFromSelectedActor(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("editor.set_selected_skeletal_animation"))
		{
			HandleTool_EditorSetSelectedSkeletalAnimation(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("level.save_current"))
		{
			HandleTool_LevelSaveCurrent(OnComplete);
			return;
		}
		if (ToolName == TEXT("level.spawn_actor"))
		{
			HandleTool_LevelSpawnActor(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("asset.search"))
		{
			HandleTool_AssetSearch(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("asset.create_blueprint"))
		{
			HandleTool_AssetCreateBlueprint(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("blueprint.get_graph_t3d"))
		{
			HandleTool_BlueprintGetGraphT3D(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("blueprint.paste_t3d"))
		{
			HandleTool_BlueprintPasteT3D(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.create_widget_blueprint"))
		{
			HandleTool_UmgCreateWidgetBlueprint(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.add_widget"))
		{
			HandleTool_UmgAddWidget(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.set_text"))
		{
			HandleTool_UmgSetText(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.set_properties"))
		{
			HandleTool_UmgSetProperties(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.scaffold_layout"))
		{
			HandleTool_UmgScaffoldLayout(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.bind_event"))
		{
			HandleTool_UmgBindEvent(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.bind_property"))
		{
			HandleTool_UmgBindProperty(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.unbind"))
		{
			HandleTool_UmgUnbind(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.list_widgets"))
		{
			HandleTool_UmgListWidgets(InputObj, OnComplete);
			return;
		}
		if (ToolName == TEXT("umg.compile"))
		{
			HandleTool_UmgCompile(InputObj, OnComplete);
			return;
		}

		auto Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Unknown tool"));
		Out->SetStringField(TEXT("toolName"), ToolName);
		OnComplete(JsonResponse(Out, 404));
	});

	return true;
}

bool FUEAgentBridgeModule::HandleTool_ProjectGetDirectory(const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("result"), FPaths::ProjectDir());
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_ProjectGetName(const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("result"), FApp::GetProjectName());
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_EditorGetSelectedActors(const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);

	TArray<TSharedPtr<FJsonValue>> Names;
	if (GEditor)
	{
		USelection* Sel = GEditor->GetSelectedActors();
		for (FSelectionIterator It(*Sel); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				Names.Add(MakeShared<FJsonValueString>(Actor->GetName()));
			}
		}
	}
	Out->SetArrayField(TEXT("result"), Names);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_EditorGetWorldInfo(const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);

	UWorld* World = nullptr;
	if (GEditor)
	{
		World = GEditor->GetEditorWorldContext().World();
	}

	Out->SetStringField(TEXT("mapName"), World ? World->GetMapName() : TEXT(""));
	Out->SetNumberField(TEXT("actorCount"), World ? World->GetActorCount() : 0);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_EditorCreateBlueprintFromSelectedActor(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString AssetPath;
	if (!Input->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing assetPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	if (!GEditor)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("GEditor unavailable"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	USelection* Sel = GEditor->GetSelectedActors();
	if (!Sel || Sel->Num() != 1)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Select exactly one actor"));
		Out->SetNumberField(TEXT("selectedCount"), Sel ? Sel->Num() : 0);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	AActor* Actor = nullptr;
	for (FSelectionIterator It(*Sel); It; ++It)
	{
		Actor = Cast<AActor>(*It);
		break;
	}
	if (!Actor)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Selected object is not an actor"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	bool bReplace = true;
	Input->TryGetBoolField(TEXT("replaceActor"), bReplace);
	bool bOpenBP = false;
	Input->TryGetBoolField(TEXT("openBlueprint"), bOpenBP);

	FKismetEditorUtilities::FCreateBlueprintFromActorParams Params;
	Params.bReplaceActor = bReplace;
	Params.bOpenBlueprint = bOpenBP;

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprintFromActor(AssetPath, Actor, Params);
	if (!BP)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to create blueprint from actor"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	if (UPackage* Package = BP->GetOutermost())
	{
		Package->MarkPackageDirty();
	}

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("result"), BP->GetPathName());
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_EditorGetSelectedActorDetails(const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);

	TArray<TSharedPtr<FJsonValue>> Items;
	if (GEditor)
	{
		USelection* Sel = GEditor->GetSelectedActors();
		for (FSelectionIterator It(*Sel); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			if (!Actor)
			{
				continue;
			}

			auto Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Actor->GetName());
			Obj->SetStringField(TEXT("path"), Actor->GetPathName());
			Obj->SetStringField(TEXT("classPath"), Actor->GetClass() ? Actor->GetClass()->GetPathName() : TEXT(""));

			USkeletalMeshComponent* Skel = Actor->FindComponentByClass<USkeletalMeshComponent>();
			Obj->SetBoolField(TEXT("hasSkeletalMesh"), Skel != nullptr);
			if (Skel)
			{
				Obj->SetStringField(TEXT("skeletalMeshPath"), Skel->GetSkeletalMeshAsset() ? Skel->GetSkeletalMeshAsset()->GetPathName() : TEXT(""));
				Obj->SetStringField(TEXT("animationMode"), StaticEnum<EAnimationMode::Type>()->GetNameStringByValue((int64)Skel->GetAnimationMode()));
				Obj->SetStringField(TEXT("animInstanceClassPath"), Skel->GetAnimInstance() && Skel->GetAnimInstance()->GetClass() ? Skel->GetAnimInstance()->GetClass()->GetPathName() : TEXT(""));
			}

			Items.Add(MakeShared<FJsonValueObject>(Obj));
		}
	}

	Out->SetArrayField(TEXT("result"), Items);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_EditorSetSelectedSkeletalAnimation(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	if (!GEditor)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("GEditor unavailable"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	USelection* Sel = GEditor->GetSelectedActors();
	if (!Sel || Sel->Num() != 1)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Select exactly one actor"));
		Out->SetNumberField(TEXT("selectedCount"), Sel ? Sel->Num() : 0);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	AActor* Actor = nullptr;
	for (FSelectionIterator It(*Sel); It; ++It)
	{
		Actor = Cast<AActor>(*It);
		break;
	}
	if (!Actor)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Selected object is not an actor"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	USkeletalMeshComponent* Skel = Actor->FindComponentByClass<USkeletalMeshComponent>();
	if (!Skel)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Actor has no SkeletalMeshComponent"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString AnimBPClassPath;
	FString AnimAssetPath;
	Input->TryGetStringField(TEXT("animBlueprintClassPath"), AnimBPClassPath);
	Input->TryGetStringField(TEXT("animationAssetPath"), AnimAssetPath);
	if (AnimBPClassPath.IsEmpty() && AnimAssetPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Provide animBlueprintClassPath or animationAssetPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	Actor->Modify();
	Skel->Modify();

	if (!AnimBPClassPath.IsEmpty())
	{
		UClass* AnimClass = StaticLoadClass(UAnimInstance::StaticClass(), nullptr, *AnimBPClassPath);
		if (!AnimClass)
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"), TEXT("Failed to load anim blueprint class"));
			Out->SetStringField(TEXT("animBlueprintClassPath"), AnimBPClassPath);
			OnComplete(JsonResponse(Out, 400));
			return true;
		}

		Skel->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		Skel->SetAnimInstanceClass(AnimClass);
		Out->SetStringField(TEXT("applied"), TEXT("animBlueprintClassPath"));
		Out->SetStringField(TEXT("value"), AnimClass->GetPathName());
	}
	else
	{
		UAnimationAsset* Anim = LoadObject<UAnimationAsset>(nullptr, *AnimAssetPath);
		if (!Anim)
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"), TEXT("Failed to load animation asset"));
			Out->SetStringField(TEXT("animationAssetPath"), AnimAssetPath);
			OnComplete(JsonResponse(Out, 400));
			return true;
		}

		Skel->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Skel->PlayAnimation(Anim, true);
		Out->SetStringField(TEXT("applied"), TEXT("animationAssetPath"));
		Out->SetStringField(TEXT("value"), Anim->GetPathName());
	}

	Out->SetBoolField(TEXT("ok"), true);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_LevelSaveCurrent(const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), true);

	bool bSaved = false;
	if (GEditor)
	{
		bSaved = FEditorFileUtils::SaveCurrentLevel();
	}

	Out->SetBoolField(TEXT("result"), bSaved);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

static bool ReadVec3(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (!Obj->TryGetObjectField(Field, Sub) || !Sub || !Sub->IsValid())
	{
		return false;
	}
	double X = 0, Y = 0, Z = 0;
	(*Sub)->TryGetNumberField(TEXT("x"), X);
	(*Sub)->TryGetNumberField(TEXT("y"), Y);
	(*Sub)->TryGetNumberField(TEXT("z"), Z);
	Out = FVector((float)X, (float)Y, (float)Z);
	return true;
}

bool FUEAgentBridgeModule::HandleTool_LevelSpawnActor(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();

	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString ClassPath;
	if (!Input->TryGetStringField(TEXT("classPath"), ClassPath) || ClassPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing classPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWorld* World = (GEditor ? GEditor->GetEditorWorldContext().World() : nullptr);
	if (!World)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("No editor world"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	UClass* SpawnClass = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
	if (!SpawnClass)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load class"));
		Out->SetStringField(TEXT("classPath"), ClassPath);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FVector Location(0, 0, 0);
	FVector Scale(1, 1, 1);
	ReadVec3(Input, TEXT("location"), Location);
	ReadVec3(Input, TEXT("scale"), Scale);

	FRotator Rot(0, 0, 0);
	{
		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		if (Input->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj && RotObj->IsValid())
		{
			double Pitch = 0, Yaw = 0, Roll = 0;
			(*RotObj)->TryGetNumberField(TEXT("pitch"), Pitch);
			(*RotObj)->TryGetNumberField(TEXT("yaw"), Yaw);
			(*RotObj)->TryGetNumberField(TEXT("roll"), Roll);
			Rot = FRotator((float)Pitch, (float)Yaw, (float)Roll);
		}
	}

	FTransform T(Rot, Location, Scale);

	FActorSpawnParameters Params;
	FString Name;
	if (Input->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
	{
		Params.Name = FName(*Name);
	}

	AActor* Actor = World->SpawnActor<AActor>(SpawnClass, T, Params);
	if (!Actor)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Spawn failed"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("result"), Actor->GetPathName());
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_AssetSearch(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString PackagePath;
	if (!Input->TryGetStringField(TEXT("packagePath"), PackagePath) || PackagePath.IsEmpty())
	{
		PackagePath = TEXT("/Game");
	}
	if (!PackagePath.StartsWith(TEXT("/")))
	{
		PackagePath = TEXT("/") + PackagePath;
	}

	double LimitNum = 50;
	Input->TryGetNumberField(TEXT("limit"), LimitNum);
	const int32 Limit = FMath::Clamp((int32)LimitNum, 1, 500);

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(FName(*PackagePath));

	const TArray<TSharedPtr<FJsonValue>>* ClassPaths = nullptr;
	if (Input->TryGetArrayField(TEXT("classPaths"), ClassPaths) && ClassPaths)
	{
		for (const TSharedPtr<FJsonValue>& V : *ClassPaths)
		{
			const FString S = V.IsValid() ? V->AsString() : FString();
			if (!S.IsEmpty())
			{
				FTopLevelAssetPath P(S);
				if (P.IsValid())
				{
					Filter.ClassPaths.Add(P);
				}
			}
		}
	}

	IAssetRegistry& AR = FAssetRegistryModule::GetRegistry();
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Items;
	int32 Count = 0;
	for (const FAssetData& A : Assets)
	{
		if (Count >= Limit)
		{
			break;
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("objectPath"), A.ToSoftObjectPath().ToString());
		Obj->SetStringField(TEXT("packageName"), A.PackageName.ToString());
		Obj->SetStringField(TEXT("assetName"), A.AssetName.ToString());
		Obj->SetStringField(TEXT("classPath"), A.AssetClassPath.ToString());
		Items.Add(MakeShared<FJsonValueObject>(Obj));
		Count++;
	}

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetArrayField(TEXT("result"), Items);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_AssetCreateBlueprint(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();

	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString AssetPath;
	if (!Input->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing assetPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString ParentClassPath(TEXT("/Script/Engine.Actor"));
	Input->TryGetStringField(TEXT("parentClassPath"), ParentClassPath);

	UClass* ParentClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ParentClassPath);
	if (!ParentClass)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load parent class"));
		Out->SetStringField(TEXT("parentClassPath"), ParentClassPath);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString PackageName = AssetPath;
	FString AssetName;
	AssetPath.Split(TEXT("/"), &PackageName, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (AssetName.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Invalid assetPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UPackage* Package = CreatePackage(*AssetPath);
	if (!Package)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to create package"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		EBlueprintType::BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("UEAgentBridge"))
	);

	if (!BP)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to create blueprint"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	FKismetEditorUtilities::CompileBlueprint(BP);
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(BP);

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("result"), BP->GetPathName());
	OnComplete(JsonResponse(Out, 200));
	return true;
}

static UBlueprint* LoadBlueprintByPath(const FString& BlueprintPath)
{
	return LoadObject<UBlueprint>(nullptr, *BlueprintPath);
}

static UEdGraph* FindBlueprintGraph(UBlueprint* BP, const FString& GraphName)
{
	if (!BP)
	{
		return nullptr;
	}

	auto MatchByName = [&GraphName](UEdGraph* G)
	{
		return G && G->GetName() == GraphName;
	};

	for (UEdGraph* G : BP->UbergraphPages)
	{
		if (MatchByName(G))
		{
			return G;
		}
	}
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (MatchByName(G))
		{
			return G;
		}
	}
	for (UEdGraph* G : BP->MacroGraphs)
	{
		if (MatchByName(G))
		{
			return G;
		}
	}

	if (GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase) && BP->UbergraphPages.Num() > 0)
	{
		return BP->UbergraphPages[0];
	}

	return nullptr;
}

bool FUEAgentBridgeModule::HandleTool_BlueprintGetGraphT3D(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();

	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath;
	FString GraphName;
	if (!Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.IsEmpty() ||
		!Input->TryGetStringField(TEXT("graphName"), GraphName) || GraphName.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath or graphName"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UBlueprint* BP = LoadBlueprintByPath(BlueprintPath);
	if (!BP)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load blueprint"));
		Out->SetStringField(TEXT("blueprintPath"), BlueprintPath);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UEdGraph* Graph = FindBlueprintGraph(BP, GraphName);
	if (!Graph)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Graph not found"));
		Out->SetStringField(TEXT("graphName"), GraphName);
		OnComplete(JsonResponse(Out, 404));
		return true;
	}

	TSet<UObject*> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Nodes.Add(Node);
		}
	}

	FString T3D;
	FEdGraphUtilities::ExportNodesToText(Nodes, T3D);
	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("result"), T3D);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

static void ClearGraphNodes(UEdGraph* Graph)
{
	if (!Graph)
	{
		return;
	}

	TArray<UEdGraphNode*> NodesCopy = Graph->Nodes;
	for (UEdGraphNode* Node : NodesCopy)
	{
		if (Node)
		{
			Node->Modify();
			Node->DestroyNode();
		}
	}
}

bool FUEAgentBridgeModule::HandleTool_BlueprintPasteT3D(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();

	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath;
	FString GraphName;
	FString T3D;
	if (!Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.IsEmpty() ||
		!Input->TryGetStringField(TEXT("graphName"), GraphName) || GraphName.IsEmpty() ||
		!Input->TryGetStringField(TEXT("t3d"), T3D) || T3D.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath, graphName, or t3d"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	bool bClearExisting = false;
	Input->TryGetBoolField(TEXT("clearExisting"), bClearExisting);
	bool bCompile = true;
	Input->TryGetBoolField(TEXT("compile"), bCompile);

	UBlueprint* BP = LoadBlueprintByPath(BlueprintPath);
	if (!BP)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load blueprint"));
		Out->SetStringField(TEXT("blueprintPath"), BlueprintPath);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UEdGraph* Graph = FindBlueprintGraph(BP, GraphName);
	if (!Graph)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Graph not found"));
		Out->SetStringField(TEXT("graphName"), GraphName);
		OnComplete(JsonResponse(Out, 404));
		return true;
	}

	BP->Modify();
	Graph->Modify();

	if (bClearExisting)
	{
		ClearGraphNodes(Graph);
	}

	TSet<UEdGraphNode*> Imported;
	FEdGraphUtilities::ImportNodesFromText(Graph, T3D, Imported);

	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(BP);
	}

	if (UPackage* Package = BP->GetOutermost())
	{
		Package->MarkPackageDirty();
	}

	TArray<TSharedPtr<FJsonValue>> ImportedGuids;
	for (UEdGraphNode* Node : Imported)
	{
		if (Node)
		{
			ImportedGuids.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
		}
	}

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetArrayField(TEXT("importedNodeGuids"), ImportedGuids);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

static FString NormalizeAssetPath(const FString& In)
{
	FString Out = In;
	// If the user passed "/Game/UI/WBP.WBP", strip object suffix for package path usage.
	int32 DotIndex = INDEX_NONE;
	if (Out.FindChar(TEXT('.'), DotIndex))
	{
		Out = Out.Left(DotIndex);
	}
	return Out;
}

static bool SplitPackageAndName(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
{
	const FString Normalized = NormalizeAssetPath(AssetPath);
	if (!Normalized.StartsWith(TEXT("/")))
	{
		return false;
	}
	FString Left, Right;
	if (!Normalized.Split(TEXT("/"), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		return false;
	}
	OutPackagePath = Left;
	OutAssetName = Right;
	return !OutPackagePath.IsEmpty() && !OutAssetName.IsEmpty();
}

static UWidgetBlueprint* LoadWidgetBlueprintByPath(const FString& BlueprintPath)
{
	return LoadObject<UWidgetBlueprint>(nullptr, *BlueprintPath);
}

static bool ReadVec2(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector2D& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (!Obj->TryGetObjectField(Field, Sub) || !Sub || !Sub->IsValid())
	{
		return false;
	}
	double X = 0, Y = 0;
	(*Sub)->TryGetNumberField(TEXT("x"), X);
	(*Sub)->TryGetNumberField(TEXT("y"), Y);
	Out = FVector2D((float)X, (float)Y);
	return true;
}

static bool ReadLinearColor(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FLinearColor& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Sub = nullptr;
	if (!Obj->TryGetObjectField(Field, Sub) || !Sub || !Sub->IsValid())
	{
		return false;
	}
	double R = 1, G = 1, B = 1, A = 1;
	(*Sub)->TryGetNumberField(TEXT("r"), R);
	(*Sub)->TryGetNumberField(TEXT("g"), G);
	(*Sub)->TryGetNumberField(TEXT("b"), B);
	(*Sub)->TryGetNumberField(TEXT("a"), A);
	Out = FLinearColor(
		FMath::Clamp((float)R, 0.0f, 1.0f),
		FMath::Clamp((float)G, 0.0f, 1.0f),
		FMath::Clamp((float)B, 0.0f, 1.0f),
		FMath::Clamp((float)A, 0.0f, 1.0f));
	return true;
}

static bool TryParseVisibility(const FString& In, ESlateVisibility& Out)
{
	if (In.Equals(TEXT("Visible"), ESearchCase::IgnoreCase))
	{
		Out = ESlateVisibility::Visible;
		return true;
	}
	if (In.Equals(TEXT("Collapsed"), ESearchCase::IgnoreCase))
	{
		Out = ESlateVisibility::Collapsed;
		return true;
	}
	if (In.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase))
	{
		Out = ESlateVisibility::Hidden;
		return true;
	}
	if (In.Equals(TEXT("HitTestInvisible"), ESearchCase::IgnoreCase))
	{
		Out = ESlateVisibility::HitTestInvisible;
		return true;
	}
	if (In.Equals(TEXT("SelfHitTestInvisible"), ESearchCase::IgnoreCase))
	{
		Out = ESlateVisibility::SelfHitTestInvisible;
		return true;
	}
	return false;
}

static void ApplyCanvasLayoutObject(const TSharedPtr<FJsonObject>& LayoutObj, UWidget* Widget)
{
	if (!LayoutObj.IsValid() || !Widget)
	{
		return;
	}

	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
	{
		FVector2D Pos(0, 0), Size(200, 50), AnchorMin(0, 0), AnchorMax(0, 0), Align(0, 0);
		ReadVec2(LayoutObj, TEXT("position"), Pos);
		ReadVec2(LayoutObj, TEXT("size"), Size);
		ReadVec2(LayoutObj, TEXT("anchorMin"), AnchorMin);
		ReadVec2(LayoutObj, TEXT("anchorMax"), AnchorMax);
		ReadVec2(LayoutObj, TEXT("alignment"), Align);

		CanvasSlot->SetPosition(Pos);
		CanvasSlot->SetSize(Size);
		CanvasSlot->SetAnchors(FAnchors(AnchorMin.X, AnchorMin.Y, AnchorMax.X, AnchorMax.Y));
		CanvasSlot->SetAlignment(Align);

		double Z = 0;
		if (LayoutObj->TryGetNumberField(TEXT("zOrder"), Z))
		{
			CanvasSlot->SetZOrder((int32)Z);
		}
	}
}

static UCanvasPanel* CreateCanvasRoot(UWidgetBlueprint* WBP)
{
	if (!WBP || !WBP->WidgetTree)
	{
		return nullptr;
	}

	const FName RootName = MakeUniqueObjectName(WBP->WidgetTree, UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
	UCanvasPanel* Canvas = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), RootName);
	WBP->WidgetTree->RootWidget = Canvas;
	return Canvas;
}

static bool AttachWidgetToTree(UWidgetBlueprint* WBP, UWidget* NewWidget, const FString& ParentName, bool bWrapRootInCanvas, FString& OutError)
{
	if (!WBP || !WBP->WidgetTree || !NewWidget)
	{
		OutError = TEXT("Invalid WidgetBlueprint or widget");
		return false;
	}

	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* ParentWidget = nullptr;
	if (!ParentName.IsEmpty())
	{
		ParentWidget = Tree->FindWidget(FName(*ParentName));
		if (!ParentWidget)
		{
			OutError = TEXT("parentName not found");
			return false;
		}
	}

	auto TryAttachTo = [&](UWidget* Parent) -> bool
	{
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Parent))
		{
			Panel->AddChild(NewWidget);
			return true;
		}
		if (UContentWidget* Content = Cast<UContentWidget>(Parent))
		{
			if (Content->GetContent() != nullptr)
			{
				OutError = TEXT("Content widget already has content");
				return false;
			}
			Content->SetContent(NewWidget);
			return true;
		}
		OutError = TEXT("Parent widget is not a panel/content widget");
		return false;
	};

	if (ParentWidget)
	{
		return TryAttachTo(ParentWidget);
	}

	// No explicit parent: attach to root if possible, otherwise create/wrap a Canvas root.
	if (!Tree->RootWidget)
	{
		if (NewWidget->IsA<UPanelWidget>() || NewWidget->IsA<UContentWidget>())
		{
			Tree->RootWidget = NewWidget;
			return true;
		}

		UCanvasPanel* Canvas = CreateCanvasRoot(WBP);
		if (!Canvas)
		{
			OutError = TEXT("Failed to create Canvas root");
			return false;
		}
		Canvas->AddChild(NewWidget);
		return true;
	}

	if (TryAttachTo(Tree->RootWidget))
	{
		return true;
	}

	if (bWrapRootInCanvas)
	{
		UWidget* ExistingRoot = Tree->RootWidget;
		UCanvasPanel* Canvas = CreateCanvasRoot(WBP);
		if (!Canvas)
		{
			OutError = TEXT("Failed to wrap root in Canvas");
			return false;
		}
		Canvas->AddChild(ExistingRoot);
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(ExistingRoot->Slot))
		{
			Slot->SetPosition(FVector2D(0, 0));
			Slot->SetSize(FVector2D(0, 0));
			Slot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
			Slot->SetAlignment(FVector2D(0, 0));
		}
		Canvas->AddChild(NewWidget);
		return true;
	}

	OutError = TEXT("Could not attach widget. Specify parentName or set wrapRootInCanvas=true.");
	return false;
}

bool FUEAgentBridgeModule::HandleTool_UmgCreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString AssetPath;
	if (!Input->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing assetPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString ParentClassPath(TEXT("/Script/UMG.UserWidget"));
	Input->TryGetStringField(TEXT("parentClassPath"), ParentClassPath);
	UClass* ParentClass = StaticLoadClass(UUserWidget::StaticClass(), nullptr, *ParentClassPath);
	if (!ParentClass)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load parent class"));
		Out->SetStringField(TEXT("parentClassPath"), ParentClassPath);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString PackagePath;
	FString AssetName;
	if (!SplitPackageAndName(AssetPath, PackagePath, AssetName))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Invalid assetPath. Use /Game/.../Name"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = ParentClass;

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	UObject* Created = AssetTools.CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Created);
	if (!WBP)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to create WidgetBlueprint"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	FKismetEditorUtilities::CompileBlueprint(WBP);
	if (UPackage* Package = WBP->GetOutermost())
	{
		Package->MarkPackageDirty();
	}

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("result"), WBP->GetPathName());
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgAddWidget(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath;
	FString WidgetClassPath;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	Input->TryGetStringField(TEXT("widgetClassPath"), WidgetClassPath);
	if (BlueprintPath.IsEmpty() || WidgetClassPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath or widgetClassPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	if (!WBP || !WBP->WidgetTree)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load WidgetBlueprint"));
		Out->SetStringField(TEXT("blueprintPath"), BlueprintPath);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UClass* WidgetClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *WidgetClassPath);
	if (!WidgetClass)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load widget class"));
		Out->SetStringField(TEXT("widgetClassPath"), WidgetClassPath);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString NameStr;
	Input->TryGetStringField(TEXT("name"), NameStr);
	const FName WidgetName = NameStr.IsEmpty() ? NAME_None : FName(*NameStr);

	WBP->Modify();
	WBP->WidgetTree->Modify();

	UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetName);
	if (!NewWidget)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to construct widget"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	FString ParentName;
	Input->TryGetStringField(TEXT("parentName"), ParentName);
	bool bWrapRootInCanvas = false;
	Input->TryGetBoolField(TEXT("wrapRootInCanvas"), bWrapRootInCanvas);

	FString AttachError;
	if (!AttachWidgetToTree(WBP, NewWidget, ParentName, bWrapRootInCanvas, AttachError))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), AttachError.IsEmpty() ? TEXT("Could not attach widget") : AttachError);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	// Optional CanvasPanel layout
	const TSharedPtr<FJsonObject>* LayoutObj = nullptr;
	if (Input->TryGetObjectField(TEXT("canvasLayout"), LayoutObj) && LayoutObj && LayoutObj->IsValid())
	{
		ApplyCanvasLayoutObject(*LayoutObj, NewWidget);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetStringField(TEXT("result"), NewWidget->GetName());
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgSetText(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath, WidgetName, Text;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	Input->TryGetStringField(TEXT("widgetName"), WidgetName);
	Input->TryGetStringField(TEXT("text"), Text);
	if (BlueprintPath.IsEmpty() || WidgetName.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath or widgetName"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	if (!WBP || !WBP->WidgetTree)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load WidgetBlueprint"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!W)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Widget not found"));
		Out->SetStringField(TEXT("widgetName"), WidgetName);
		OnComplete(JsonResponse(Out, 404));
		return true;
	}

	WBP->Modify();
	W->Modify();

	if (UTextBlock* TB = Cast<UTextBlock>(W))
	{
		TB->SetText(FText::FromString(Text));
	}
	else if (URichTextBlock* RT = Cast<URichTextBlock>(W))
	{
		RT->SetText(FText::FromString(Text));
	}
	else
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Widget is not a TextBlock/RichTextBlock"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	Out->SetBoolField(TEXT("ok"), true);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

static bool ApplyUmgProperties(UWidgetBlueprint* WBP, UWidget* Widget, const TSharedPtr<FJsonObject>& PropsObj, FString& OutError)
{
	if (!WBP || !WBP->WidgetTree || !Widget || !PropsObj.IsValid())
	{
		OutError = TEXT("Invalid WidgetBlueprint/widget/properties");
		return false;
	}

	static const TSet<FString> KnownKeys = {
		TEXT("text"),
		TEXT("visibility"),
		TEXT("isEnabled"),
		TEXT("renderOpacity"),
		TEXT("colorAndOpacity"),
		TEXT("fontSize"),
		TEXT("tintColor"),
		TEXT("texturePath"),
		TEXT("canvasLayout")
	};

	TArray<FString> Unknown;
	for (const auto& Pair : PropsObj->Values)
	{
		if (!KnownKeys.Contains(Pair.Key))
		{
			Unknown.Add(Pair.Key);
		}
	}
	if (Unknown.Num() > 0)
	{
		OutError = FString::Printf(TEXT("Unsupported properties: %s"), *FString::Join(Unknown, TEXT(", ")));
		return false;
	}

	WBP->Modify();
	Widget->Modify();

	FString Vis;
	if (PropsObj->TryGetStringField(TEXT("visibility"), Vis) && !Vis.IsEmpty())
	{
		ESlateVisibility V;
		if (!TryParseVisibility(Vis, V))
		{
			OutError = TEXT("Invalid visibility. Use Visible/Hidden/Collapsed/HitTestInvisible/SelfHitTestInvisible");
			return false;
		}
		Widget->SetVisibility(V);
	}

	bool bEnabled = true;
	if (PropsObj->TryGetBoolField(TEXT("isEnabled"), bEnabled))
	{
		Widget->SetIsEnabled(bEnabled);
	}

	double Opacity = 1.0;
	if (PropsObj->TryGetNumberField(TEXT("renderOpacity"), Opacity))
	{
		Widget->SetRenderOpacity((float)FMath::Clamp(Opacity, 0.0, 1.0));
	}

	// Text + text styling (TextBlock/RichTextBlock)
	FString TextStr;
	const bool bHasText = PropsObj->TryGetStringField(TEXT("text"), TextStr);
	const bool bHasFontSize = PropsObj->HasField(TEXT("fontSize"));
	const bool bHasColor = PropsObj->HasField(TEXT("colorAndOpacity"));

	if (bHasText || bHasFontSize || bHasColor)
	{
		if (UTextBlock* TB = Cast<UTextBlock>(Widget))
		{
			if (bHasText)
			{
				TB->SetText(FText::FromString(TextStr));
			}

			if (bHasColor)
			{
				FLinearColor C;
				if (ReadLinearColor(PropsObj, TEXT("colorAndOpacity"), C))
				{
					TB->SetColorAndOpacity(FSlateColor(C));
				}
			}

			if (bHasFontSize)
			{
				double Size = 0;
				if (PropsObj->TryGetNumberField(TEXT("fontSize"), Size))
				{
					FSlateFontInfo Font = TB->GetFont();
					Font.Size = (int32)FMath::Max(1.0, Size);
					TB->SetFont(Font);
				}
			}
		}
		else if (URichTextBlock* RT = Cast<URichTextBlock>(Widget))
		{
			if (bHasText)
			{
				RT->SetText(FText::FromString(TextStr));
			}

			if (bHasColor)
			{
				FLinearColor C;
				if (ReadLinearColor(PropsObj, TEXT("colorAndOpacity"), C))
				{
					RT->SetDefaultColorAndOpacity(FSlateColor(C));
				}
			}

			if (bHasFontSize)
			{
				double Size = 0;
				if (PropsObj->TryGetNumberField(TEXT("fontSize"), Size))
				{
					FSlateFontInfo Font = RT->GetDefaultTextStyle().Font;
					Font.Size = (int32)FMath::Max(1.0, Size);
					RT->SetDefaultFont(Font);
				}
			}
		}
		else
		{
			OutError = TEXT("text/colorAndOpacity/fontSize are only supported for TextBlock/RichTextBlock");
			return false;
		}
	}

	// Image styling (Image)
	const bool bHasTint = PropsObj->HasField(TEXT("tintColor"));
	const bool bHasTexture = PropsObj->HasField(TEXT("texturePath"));
	if (bHasTint || bHasTexture)
	{
		if (UImage* Img = Cast<UImage>(Widget))
		{
			if (bHasTint)
			{
				FLinearColor C;
				if (ReadLinearColor(PropsObj, TEXT("tintColor"), C))
				{
					Img->SetColorAndOpacity(C);
				}
			}

			if (bHasTexture)
			{
				FString TexPath;
				if (PropsObj->TryGetStringField(TEXT("texturePath"), TexPath) && !TexPath.IsEmpty())
				{
					UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *TexPath);
					if (!Tex)
					{
						OutError = TEXT("Failed to load texturePath");
						return false;
					}
					Img->SetBrushFromTexture(Tex, true);
				}
			}
		}
		else if (UBorder* Border = Cast<UBorder>(Widget))
		{
			if (bHasTint)
			{
				FLinearColor C;
				if (ReadLinearColor(PropsObj, TEXT("tintColor"), C))
				{
					Border->SetBrushColor(C);
				}
			}
		}
		else
		{
			OutError = TEXT("tintColor/texturePath are only supported for Image (and tintColor for Border)");
			return false;
		}
	}

	// Canvas slot layout
	const TSharedPtr<FJsonObject>* LayoutObj = nullptr;
	if (PropsObj->TryGetObjectField(TEXT("canvasLayout"), LayoutObj) && LayoutObj && LayoutObj->IsValid())
	{
		if (Widget->Slot)
		{
			Widget->Slot->Modify();
		}
		ApplyCanvasLayoutObject(*LayoutObj, Widget);
	}

	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgSetProperties(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath, WidgetName;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	Input->TryGetStringField(TEXT("widgetName"), WidgetName);
	if (BlueprintPath.IsEmpty() || WidgetName.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath or widgetName"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
	if (!Input->TryGetObjectField(TEXT("properties"), PropsObjPtr) || !PropsObjPtr || !PropsObjPtr->IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing properties object"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	if (!WBP || !WBP->WidgetTree)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load WidgetBlueprint"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidget* W = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!W)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Widget not found"));
		Out->SetStringField(TEXT("widgetName"), WidgetName);
		OnComplete(JsonResponse(Out, 404));
		return true;
	}

	FString Err;
	if (!ApplyUmgProperties(WBP, W, *PropsObjPtr, Err))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), Err.IsEmpty() ? TEXT("Failed to set properties") : Err);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

	bool bCompile = false;
	Input->TryGetBoolField(TEXT("compile"), bCompile);
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(WBP);
		if (UPackage* Package = WBP->GetOutermost())
		{
			Package->MarkPackageDirty();
		}
	}

	Out->SetBoolField(TEXT("ok"), true);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgScaffoldLayout(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	if (BlueprintPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
	if (!Input->TryGetArrayField(TEXT("elements"), Elements) || !Elements)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing elements array"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	if (!WBP || !WBP->WidgetTree)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load WidgetBlueprint"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	WBP->Modify();
	WBP->WidgetTree->Modify();

	TArray<TSharedPtr<FJsonValue>> Created;
	for (const TSharedPtr<FJsonValue>& ElemVal : *Elements)
	{
		if (!ElemVal.IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject> ElemObj = ElemVal->AsObject();
		if (!ElemObj.IsValid())
		{
			continue;
		}

		FString WidgetClassPath;
		ElemObj->TryGetStringField(TEXT("widgetClassPath"), WidgetClassPath);
		if (WidgetClassPath.IsEmpty())
		{
			continue;
		}

		UClass* WidgetClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *WidgetClassPath);
		if (!WidgetClass)
		{
			continue;
		}

		FString NameStr;
		ElemObj->TryGetStringField(TEXT("name"), NameStr);
		const FName WidgetName = NameStr.IsEmpty() ? NAME_None : FName(*NameStr);
		UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetName);
		if (!NewWidget)
		{
			continue;
		}

		FString ParentName;
		ElemObj->TryGetStringField(TEXT("parentName"), ParentName);

		FString AttachError;
		if (!AttachWidgetToTree(WBP, NewWidget, ParentName, true, AttachError))
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"), AttachError.IsEmpty() ? TEXT("Failed to attach widget") : AttachError);
			OnComplete(JsonResponse(Out, 400));
			return true;
		}

		const TSharedPtr<FJsonObject>* LayoutObj = nullptr;
		if (ElemObj->TryGetObjectField(TEXT("canvasLayout"), LayoutObj) && LayoutObj && LayoutObj->IsValid())
		{
			ApplyCanvasLayoutObject(*LayoutObj, NewWidget);
		}

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (ElemObj->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && PropsObj->IsValid())
		{
			FString Err;
			if (!ApplyUmgProperties(WBP, NewWidget, *PropsObj, Err))
			{
				Out->SetBoolField(TEXT("ok"), false);
				Out->SetStringField(TEXT("error"), Err.IsEmpty() ? TEXT("Failed to apply element properties") : Err);
				OnComplete(JsonResponse(Out, 400));
				return true;
			}
		}

		auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), NewWidget->GetName());
		Item->SetStringField(TEXT("class"), WidgetClass->GetPathName());
		Created.Add(MakeShared<FJsonValueObject>(Item));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	bool bCompile = false;
	Input->TryGetBoolField(TEXT("compile"), bCompile);
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(WBP);
		if (UPackage* Package = WBP->GetOutermost())
		{
			Package->MarkPackageDirty();
		}
	}

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetArrayField(TEXT("result"), Created);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

static bool EnsureHandlerFunctionGraph(UWidgetBlueprint* WBP, const FName HandlerFunctionName, UFunction* SignatureFunc, bool bMakePure, FString& OutError)
{
	if (!WBP)
	{
		OutError = TEXT("Invalid WidgetBlueprint");
		return false;
	}

	if (WBP->SkeletonGeneratedClass && WBP->SkeletonGeneratedClass->FindFunctionByName(HandlerFunctionName, EIncludeSuperFlag::ExcludeSuper))
	{
		return true;
	}

	// If there's already a graph with that name, don't try to recreate it.
	{
		TArray<UEdGraph*> Graphs;
		WBP->GetAllGraphs(Graphs);
		for (UEdGraph* G : Graphs)
		{
			if (G && G->GetFName() == HandlerFunctionName)
			{
				return true;
			}
		}
	}

	if (!SignatureFunc)
	{
		OutError = TEXT("Missing signature function");
		return false;
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(WBP, HandlerFunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		OutError = TEXT("Failed to create function graph");
		return false;
	}

	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(WBP, NewGraph, true, SignatureFunc);

	if (bMakePure)
	{
		if (const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>())
		{
			K2Schema->AddExtraFunctionFlags(NewGraph, FUNC_BlueprintPure | FUNC_Const);
		}
	}

	return true;
}

static bool AddBinding(UWidgetBlueprint* WBP, const FString& WidgetName, const FName& PropertyName, const FName& HandlerFunctionName, EBindingKind Kind)
{
	if (!WBP)
	{
		return false;
	}

	WBP->Modify();

	FDelegateEditorBinding Binding;
	Binding.ObjectName = WidgetName;
	Binding.PropertyName = PropertyName;
	Binding.FunctionName = HandlerFunctionName;
	Binding.Kind = Kind;

	WBP->Bindings.Remove(Binding);
	WBP->Bindings.AddUnique(Binding);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	return true;
}

static bool RemoveBinding(UWidgetBlueprint* WBP, const FString& WidgetName, const FName& PropertyName)
{
	if (!WBP)
	{
		return false;
	}

	WBP->Modify();
	FDelegateEditorBinding Binding;
	Binding.ObjectName = WidgetName;
	Binding.PropertyName = PropertyName;
	const int32 Removed = WBP->Bindings.Remove(Binding);
	if (Removed > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	}
	return Removed > 0;
}

static bool ResolveWidgetAndDelegate(
	UWidgetBlueprint* WBP,
	const FString& WidgetName,
	const FString& DelegateOrPropertyName,
	bool bIsPropertyBinding,
	/*out*/ UWidget*& OutWidget,
	/*out*/ FName& OutPropertyName,
	/*out*/ UFunction*& OutSignatureFunc,
	/*out*/ FString& OutError)
{
	OutWidget = nullptr;
	OutPropertyName = NAME_None;
	OutSignatureFunc = nullptr;

	if (!WBP || !WBP->WidgetTree)
	{
		OutError = TEXT("Failed to load WidgetBlueprint");
		return false;
	}

	OutWidget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
	if (!OutWidget)
	{
		OutError = TEXT("Widget not found");
		return false;
	}

	const FName InName(*DelegateOrPropertyName);
	OutPropertyName = InName;

	const UClass* WidgetClass = OutWidget->GetClass();
	const FDelegateProperty* DelegateProp = nullptr;
	if (bIsPropertyBinding)
	{
		DelegateProp = FindFProperty<FDelegateProperty>(WidgetClass, FName(*(DelegateOrPropertyName + TEXT("Delegate"))));
		if (!DelegateProp)
		{
			OutError = TEXT("Property is not bindable (missing <PropertyName>Delegate on widget class)");
			return false;
		}
	}
	else
	{
		DelegateProp = FindFProperty<FDelegateProperty>(WidgetClass, InName);
		if (!DelegateProp)
		{
			OutError = TEXT("Event delegate not found on widget class");
			return false;
		}
	}

	OutSignatureFunc = DelegateProp->SignatureFunction;
	if (!OutSignatureFunc)
	{
		OutError = TEXT("Delegate signature function missing");
		return false;
	}

	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgBindEvent(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath, WidgetName, EventName, HandlerNameStr;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	Input->TryGetStringField(TEXT("widgetName"), WidgetName);
	Input->TryGetStringField(TEXT("eventName"), EventName);
	Input->TryGetStringField(TEXT("handlerFunctionName"), HandlerNameStr);
	if (BlueprintPath.IsEmpty() || WidgetName.IsEmpty() || EventName.IsEmpty() || HandlerNameStr.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath/widgetName/eventName/handlerFunctionName"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	UWidget* Widget = nullptr;
	FName PropertyName = NAME_None;
	UFunction* Sig = nullptr;
	FString Err;
	if (!ResolveWidgetAndDelegate(WBP, WidgetName, EventName, false, Widget, PropertyName, Sig, Err))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), Err);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	const FName HandlerFn(*HandlerNameStr);
	bool bCreate = true;
	Input->TryGetBoolField(TEXT("createHandlerIfMissing"), bCreate);
	if (bCreate)
	{
		if (!EnsureHandlerFunctionGraph(WBP, HandlerFn, Sig, false, Err))
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"), Err);
			OnComplete(JsonResponse(Out, 400));
			return true;
		}
	}

	if (!AddBinding(WBP, WidgetName, PropertyName, HandlerFn, EBindingKind::Function))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to add binding"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	bool bCompile = false;
	Input->TryGetBoolField(TEXT("compile"), bCompile);
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(WBP);
		if (UPackage* Package = WBP->GetOutermost())
		{
			Package->MarkPackageDirty();
		}
	}

	Out->SetBoolField(TEXT("ok"), true);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgBindProperty(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath, WidgetName, PropertyNameStr, HandlerNameStr;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	Input->TryGetStringField(TEXT("widgetName"), WidgetName);
	Input->TryGetStringField(TEXT("propertyName"), PropertyNameStr);
	Input->TryGetStringField(TEXT("handlerFunctionName"), HandlerNameStr);
	if (BlueprintPath.IsEmpty() || WidgetName.IsEmpty() || PropertyNameStr.IsEmpty() || HandlerNameStr.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath/widgetName/propertyName/handlerFunctionName"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	UWidget* Widget = nullptr;
	FName PropertyName = NAME_None;
	UFunction* Sig = nullptr;
	FString Err;
	if (!ResolveWidgetAndDelegate(WBP, WidgetName, PropertyNameStr, true, Widget, PropertyName, Sig, Err))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), Err);
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	const FName HandlerFn(*HandlerNameStr);
	bool bCreate = true;
	Input->TryGetBoolField(TEXT("createHandlerIfMissing"), bCreate);
	if (bCreate)
	{
		if (!EnsureHandlerFunctionGraph(WBP, HandlerFn, Sig, true, Err))
		{
			Out->SetBoolField(TEXT("ok"), false);
			Out->SetStringField(TEXT("error"), Err);
			OnComplete(JsonResponse(Out, 400));
			return true;
		}
	}

	if (!AddBinding(WBP, WidgetName, PropertyName, HandlerFn, EBindingKind::Function))
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to add binding"));
		OnComplete(JsonResponse(Out, 500));
		return true;
	}

	bool bCompile = false;
	Input->TryGetBoolField(TEXT("compile"), bCompile);
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(WBP);
		if (UPackage* Package = WBP->GetOutermost())
		{
			Package->MarkPackageDirty();
		}
	}

	Out->SetBoolField(TEXT("ok"), true);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgUnbind(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath, WidgetName, PropertyNameStr;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	Input->TryGetStringField(TEXT("widgetName"), WidgetName);
	Input->TryGetStringField(TEXT("propertyName"), PropertyNameStr);
	if (BlueprintPath.IsEmpty() || WidgetName.IsEmpty() || PropertyNameStr.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath/widgetName/propertyName"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	if (!WBP)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load WidgetBlueprint"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	const bool bRemoved = RemoveBinding(WBP, WidgetName, FName(*PropertyNameStr));
	Out->SetBoolField(TEXT("ok"), bRemoved);
	if (!bRemoved)
	{
		Out->SetStringField(TEXT("error"), TEXT("Binding not found"));
		OnComplete(JsonResponse(Out, 404));
		return true;
	}

	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgListWidgets(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	if (BlueprintPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	if (!WBP || !WBP->WidgetTree)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load WidgetBlueprint"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	TArray<UWidget*> All;
	WBP->WidgetTree->GetAllWidgets(All);
	TArray<TSharedPtr<FJsonValue>> Items;
	for (UWidget* W : All)
	{
		if (!W) continue;
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), W->GetName());
		Obj->SetStringField(TEXT("class"), W->GetClass() ? W->GetClass()->GetPathName() : TEXT(""));
		Items.Add(MakeShared<FJsonValueObject>(Obj));
	}

	Out->SetBoolField(TEXT("ok"), true);
	Out->SetArrayField(TEXT("result"), Items);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

bool FUEAgentBridgeModule::HandleTool_UmgCompile(const TSharedPtr<FJsonObject>& Input, const FHttpResultCallback& OnComplete)
{
	auto Out = MakeShared<FJsonObject>();
	if (!Input.IsValid())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing input"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FString BlueprintPath;
	Input->TryGetStringField(TEXT("blueprintPath"), BlueprintPath);
	if (BlueprintPath.IsEmpty())
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Missing blueprintPath"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	UWidgetBlueprint* WBP = LoadWidgetBlueprintByPath(BlueprintPath);
	if (!WBP)
	{
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), TEXT("Failed to load WidgetBlueprint"));
		OnComplete(JsonResponse(Out, 400));
		return true;
	}

	FKismetEditorUtilities::CompileBlueprint(WBP);
	if (UPackage* Package = WBP->GetOutermost())
	{
		Package->MarkPackageDirty();
	}
	Out->SetBoolField(TEXT("ok"), true);
	OnComplete(JsonResponse(Out, 200));
	return true;
}

IMPLEMENT_MODULE(FUEAgentBridgeModule, UEAgentBridge)
