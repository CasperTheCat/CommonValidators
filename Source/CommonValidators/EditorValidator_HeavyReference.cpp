#include "EditorValidator_HeavyReference.h"

#include "Engine/Blueprint.h"
#include "Misc/DataValidation.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Variable.h"
#include "CommonValidatorsStatics.h"
#include "CommonValidatorsDeveloperSettings.h"
#include "K2Node.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetManagerEditor/Public/AssetManagerEditorModule.h"
#include <K2Node_DynamicCast.h>
#include <K2Node_LoadAsset.h>

#include "Engine/AssetManager.h"

#define LOCTEXT_NAMESPACE "CommonValidators"

namespace UE::Internal::HeavyReferenceValidatorHelpers
{
} // namespace UE::Internal::HeavyReferenceValidatorHelpers

bool UEditorValidator_HeavyReference::CanValidateAsset_Implementation(
	const FAssetData& InAssetData,
	UObject* InObject,
	FDataValidationContext& InContext) const
{
	bool bIsValidatorEnabled = GetDefault<UCommonValidatorsDeveloperSettings>()->bEnableHeavyReferenceValidator;
	return bIsValidatorEnabled && (InObject != nullptr) && InObject->IsA<UBlueprint>();
}


EDataValidationResult UEditorValidator_HeavyReference::ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset,
                                                                                          FDataValidationContext& Context)
{
	UBlueprint* Blueprint = Cast<UBlueprint>(InAsset);
	if (Blueprint == nullptr)
	{
		return EDataValidationResult::NotValidated;
	}
	bool bFoundBadNode = false;
	const bool bShouldError = GetDefault<UCommonValidatorsDeveloperSettings>()->bErrorHeavyReference;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<
		FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry* AssetReg = &AssetRegistryModule.Get();

	IAssetManagerEditorModule& ManagerEditorModule = IAssetManagerEditorModule::Get();
	IAssetRegistry* const AssetRegistry = &AssetRegistryModule.Get();
	UAssetManager* const AssetManager = &UAssetManager::Get();
	IAssetManagerEditorModule* const EditorModule = &IAssetManagerEditorModule::Get();
	const FAssetManagerEditorRegistrySource* const AssetRegistrySource = EditorModule->GetCurrentRegistrySource(false);

	FAssetIdentifier InAssetIdentifier;
	{
		FPrimaryAssetId PrimaryAssetId = IAssetManagerEditorModule::ExtractPrimaryAssetIdFromFakeAssetData(InAssetData);

		if (PrimaryAssetId.IsValid())
		{
			InAssetIdentifier = PrimaryAssetId;
		}
		else
		{
			InAssetIdentifier = InAssetData.PackageName;
		}
	}

	// Got assets. We want to sizemap these
	TSet<FAssetIdentifier> VisitList;
	TArray<FAssetIdentifier> FoundAssetList;
	FoundAssetList.Add(InAssetIdentifier);

	uint64 TotalSize = 0;
	//TotalSize += GatherAssetSize(VisitedAssetIdentifiers, RootAsset);

	for (uint64 i = 0; i < FoundAssetList.Num(); i++)
	{
		const FAssetIdentifier& FoundAssetId = FoundAssetList[i];
		if (VisitList.Contains(FoundAssetId))
		{
			// Cont
			continue;
		}

		VisitList.Add(FoundAssetId);

		// Size this asset first
		FName AssetPackageName = FoundAssetId.IsPackage() ? FoundAssetId.PackageName : NAME_None;
		FString AssetPackageNameString = (AssetPackageName != NAME_None) ? AssetPackageName.ToString() : FString();
		FPrimaryAssetId AssetPrimaryId = FoundAssetId.GetPrimaryAssetId();
		int32 ChunkId = UAssetManager::ExtractChunkIdFromPrimaryAssetId(AssetPrimaryId);

		// Only support packages and primary assets
		if (AssetPackageName == NAME_None && !AssetPrimaryId.IsValid())
		{
			UE_LOG(LogTemp, Display, TEXT("Asset not including in size: %s"), *FoundAssetId.PackageName.ToString())
			continue;
		}

		// Don't bother showing code references
		if (AssetPackageNameString.StartsWith(TEXT("/Script/")))
		{
			UE_LOG(LogTemp, Display, TEXT("Code Refs are defined as okie-dokie: %s"), *FoundAssetId.PackageName.ToString())
			continue;
		}

		// Set some defaults for this node. These will be used if we can't actually locate the asset.
		FAssetData ThisAssetData;
		if (AssetPackageName != NAME_None)
		{
			ThisAssetData.AssetName = AssetPackageName;
			ThisAssetData.AssetClassPath = FTopLevelAssetPath(TEXT("/None"), TEXT("MissingAsset"));

			const FString AssetPathString = AssetPackageNameString + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPackageNameString);
			FAssetData FoundData = AssetRegistrySource->GetAssetByObjectPath(FSoftObjectPath(AssetPathString));

			if (FoundData.IsValid())
			{
				ThisAssetData = MoveTemp(FoundData);
			}
		}
		else
		{
			ThisAssetData = IAssetManagerEditorModule::CreateFakeAssetDataFromPrimaryAssetId(AssetPrimaryId);
		}

		// Go for asset sizing
		if (ThisAssetData.IsValid())
		{
			FAssetManagerDependencyQuery DependencyQuery = FAssetManagerDependencyQuery::None();
			DependencyQuery.Flags = UE::AssetRegistry::EDependencyQuery::Game;

			if (AssetPackageName != NAME_None)
			{
				DependencyQuery.Categories = UE::AssetRegistry::EDependencyCategory::Package;
				DependencyQuery.Flags |= UE::AssetRegistry::EDependencyQuery::Hard;
			}
			else
			{
				// ?
				DependencyQuery.Categories = UE::AssetRegistry::EDependencyCategory::Manage;
				DependencyQuery.Flags |= UE::AssetRegistry::EDependencyQuery::Direct;
			}

			// Ignore ourselves in this calc.
			// We are not a reference: we are us.
			if (AssetPackageName != NAME_None && i > 0)
			{
				int64 FoundSize = 0;
				if (EditorModule->GetIntegerValueForCustomColumn(ThisAssetData, IAssetManagerEditorModule::DiskSizeName, FoundSize))
				{
					TotalSize += FoundSize;
				}
				else
				{
					// ?
					UE_LOG(LogTemp, Warning, TEXT("Cannot stat size for %s.%s"), *FoundAssetId.ToString(), *AssetPackageNameString);
				}
			}

			// Find lowers
			TArray<FAssetIdentifier> OutAssetData;
			AssetReg->GetDependencies(FoundAssetId, OutAssetData, DependencyQuery.Categories, DependencyQuery.Flags);

			// The TArray may have realloc'd and caused our pointer to invalidate at this point.
			// FoundAssetId is now unsafe to use.
			IAssetManagerEditorModule::Get().FilterAssetIdentifiersForCurrentRegistrySource(OutAssetData, DependencyQuery, true);
			FoundAssetList.Append(OutAssetData);

			IAssetManagerEditorModule::Get().FilterAssetIdentifiersForCurrentRegistrySource(OutAssetData, DependencyQuery, true);
		}
	}


	if (TotalSize > GetDefault<UCommonValidatorsDeveloperSettings>()->MaximumAllowedReferenceSizeKiloBytes * 1024)
	{
		// Create a tokenized message with an action to open the Blueprint and focus the node
		TSharedRef<FTokenizedMessage> TokenizedMessage = FTokenizedMessage::Create(
			(bShouldError ? EMessageSeverity::Error : EMessageSeverity::Warning),
			FText::Format(
				LOCTEXT("CommonValidators.HeavyRef.AssetWarning", "Heavy references in asset {0}!"),
				FText::FromString(InAssetIdentifier.ToString())
				)
			);

		TokenizedMessage->AddToken(FActionToken::Create(
			LOCTEXT("CommonValidators.HeavyRef.OpenBlueprint", "Open Blueprint"),
			LOCTEXT("CommonValidators.HeavyRef.OpenBlueprintDesc", "Open Blueprint"),
			FOnActionTokenExecuted::CreateLambda([Blueprint]()
			{
				UCommonValidatorsStatics::OpenBlueprint(Blueprint);
			}),
			false
			));

		Context.AddMessage(TokenizedMessage);

		return bShouldError ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
	}


	TArray<UEdGraph*> AllGraphs;
	AllGraphs.Append(Blueprint->FunctionGraphs);
	AllGraphs.Append(Blueprint->UbergraphPages);


	for (FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		auto Intro = Variable.VarType;
		if (Intro.bIsWeakPointer)
		{
			UE_LOG(LogTemp, Display, TEXT("WEAK"));
		}

		TWeakObjectPtr<UObject> VarType2 = Intro.PinValueType.TerminalSubCategoryObject;

		TWeakObjectPtr<UObject> VarType = Variable.VarType.PinSubCategoryObject;

		if (VarType.IsValid())
		{
			UE_LOG(LogTemp, Display, TEXT("%s"), *VarType->GetFullName());
		}

		// If this var type is heavy, report it
		//UE_LOG(LogTemp, Display, TEXT("%s"), *VarType->StaticClass()->GetFullName());

		//UE_LOG(LogTemp, Display, TEXT("%s"), *VarType2->StaticClass()->GetFullName());
	}

	for (UEdGraph* Graph : AllGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			// Can this node contain a class reference?
			if (Node->IsA<UK2Node_Variable>())
			{
				// Can we get the BP type reference
				auto T = Cast<UK2Node_Variable>(Node);
			}


			// Dynamic Casts
			if (Node->IsA<UK2Node_DynamicCast>())
			{
				// Can we get the BP type reference
				UK2Node_DynamicCast* DynamicCast = Cast<UK2Node_DynamicCast>(Node);
				if (DynamicCast)
				{
					TSubclassOf<UObject> TargetType = DynamicCast->TargetType;
					UE_LOG(LogTemp, Display, TEXT("%s"), *TargetType->GetFullName());
				}
			}

			//// Dynamic Casts
			//if (Node->IsA<UK2Node_LoadAsset>())
			//{
			//	// Can we get the BP type reference
			//	UK2Node_LoadAsset* AssetLoad = Cast<UK2Node_LoadAsset>(Node);
			//	if (AssetLoad)
			//	{
			//		TSubclassOf<UObject> TargetType = DynamicCast->TargetType;
			//	}
			//}


			//FReferenceCollector RC;
			////UK2Node_CallFunction D;
			//AddReferencedObjects(Node, RC);

			//         if (Node->IsA<UK2Node_BreakStruct>())
			//         {
			//             continue;
			//         }

			//         UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
			//         if (CallNode == nullptr)
			//         {
			//             continue;
			//         }

			//if (UFunction* TargetFunc = CallNode->GetTargetFunction())
			//{
			//	if (TargetFunc->HasMetaData(TEXT("NativeBreakFunc")) ||
			//		TargetFunc->HasMetaData(TEXT("NativeMakeFunc")))
			//	{
			//		continue;
			//	}
			//}
		}
	}

	if (bShouldError && bFoundBadNode)
	{
		return EDataValidationResult::Invalid;
	}

	return EDataValidationResult::Valid;
}

#undef LOCTEXT_NAMESPACE
