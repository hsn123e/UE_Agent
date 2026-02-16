#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "UEAgentBridgeSettings.generated.h"

UENUM()
enum class EUEAgentProvider : uint8
{
	OpenAICompatible UMETA(DisplayName = "OpenAI-compatible"),
	OllamaCloud UMETA(DisplayName = "Ollama Cloud"),
};

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "UE Agent Bridge"))
class UEAGENTBRIDGE_API UUEAgentBridgeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(config, EditAnywhere, Category = "LLM")
	EUEAgentProvider Provider = EUEAgentProvider::OpenAICompatible;

	// OpenAI-compatible base URL, e.g. https://api.openai.com/v1 or http://127.0.0.1:11434/v1
	UPROPERTY(config, EditAnywhere, Category = "LLM")
	FString BaseUrl = TEXT("http://127.0.0.1:11434/v1");

	// API key (Bearer). Leave empty for local servers that don't require auth.
	UPROPERTY(config, EditAnywhere, Category = "LLM", meta = (PasswordField = "true"))
	FString ApiKey;

	UPROPERTY(config, EditAnywhere, Category = "LLM")
	FString Model = TEXT("gpt-4o-mini");

	UPROPERTY(config, EditAnywhere, Category = "LLM", meta = (ClampMin = "1", ClampMax = "30"))
	int32 MaxSteps = 8;

	UPROPERTY(config, EditAnywhere, Category = "LLM", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float Temperature = 0.0f;
};
