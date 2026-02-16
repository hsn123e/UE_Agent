#include "UEAgentBTTask_FindRandomReachablePoint.h"

#include "AIController.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "NavigationSystem.h"

EBTNodeResult::Type UUEAgentBTTask_FindRandomReachablePoint::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB || DestinationKey.SelectedKeyName.IsNone())
	{
		return EBTNodeResult::Failed;
	}

	AAIController* AI = OwnerComp.GetAIOwner();
	APawn* Pawn = AI ? AI->GetPawn() : nullptr;
	if (!Pawn)
	{
		return EBTNodeResult::Failed;
	}

	UWorld* World = Pawn->GetWorld();
	if (!World)
	{
		return EBTNodeResult::Failed;
	}

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (!NavSys)
	{
		return EBTNodeResult::Failed;
	}

	FNavLocation Out;
	const FVector Origin = Pawn->GetActorLocation();
	if (!NavSys->GetRandomReachablePointInRadius(Origin, Radius, Out))
	{
		return EBTNodeResult::Failed;
	}

	BB->SetValueAsVector(DestinationKey.SelectedKeyName, Out.Location);
	return EBTNodeResult::Succeeded;
}

