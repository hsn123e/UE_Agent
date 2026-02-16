#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTreeTypes.h"

#include "UEAgentBTTask_FindRandomReachablePoint.generated.h"

UCLASS()
class UEAGENTBRIDGE_API UUEAgentBTTask_FindRandomReachablePoint : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector DestinationKey;

	UPROPERTY(EditAnywhere, Category = "AI", meta = (ClampMin = "100.0", UIMin = "100.0"))
	float Radius = 1200.0f;

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
};

