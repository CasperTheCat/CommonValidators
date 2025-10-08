// Copyright Notice
// Author

// This Header
#include "EditorValidator_HeavyReference.h"

// Unreal
#include "AssetManagerEditor/Public/AssetManagerEditorModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/DataValidation.h"

// Project

// Local
#include "CommonValidatorsDeveloperSettings.h"
#include "CommonValidatorsStatics.h"



#define LOCTEXT_NAMESPACE "CommonValidators"

namespace UE::Internal::HeavyReferenceValidatorHelpers
{
} // namespace UE::Internal::HeavyReferenceValidatorHelpers

bool UEditorValidator_HeavyReference::CanValidateAsset_Implementation(
	const FAssetData& InAssetData,
	UObject* InObject,
	FDataValidationContext& InContext) const
{
	if (!IsValid(InObject))
	{
		return false;
	}

	// Early out to prevent chewing CPU time when not enabled
	if (!GetDefault<UCommonValidatorsDeveloperSettings>()->bEnableHeavyReferenceValidator)
	{
		return false;
	}

	UBlueprint* Blueprint = Cast<UBlueprint>(InObject);
	if (!IsValid(Blueprint))
	{
		return false;
	}

	// Check if we want to run validation here
	// Remove any BPs that inherit from the classes in class and child list
	{
		const TArray<TSubclassOf<UObject>>& IgnoreChildrenList = GetDefault<UCommonValidatorsDeveloperSettings>()->HeavyValidatorClassAndChildIgnoreList;
		for (const TSubclassOf<UObject>& IgnoredChild : IgnoreChildrenList)
		{
			const UClass* const ParentClass = FBlueprintEditorUtils::FindFirstNativeClass(Blueprint->ParentClass);
			if (Blueprint->IsA(IgnoredChild) || ParentClass->IsChildOf(IgnoredChild))
			{
				return false;
			}
		}
	}
	
	// Limit to BPs for now?
	return true && InObject->IsA<UBlueprint>();
}


EDataValidationResult UEditorValidator_HeavyReference::ValidateLoadedAsset_Implementation(const FAssetData& InAssetData, UObject* InAsset,
                                                                                          FDataValidationContext& Context)
{
	// Ignore non-BP types
	UBlueprint* Blueprint = Cast<UBlueprint>(InAsset);
	if (!IsValid(Blueprint))
	{
		return EDataValidationResult::NotValidated;
	}

	// Remove any BPs that inherit from the classes in class and child list
	{
		const TArray<TSubclassOf<UObject>>& IgnoreChildrenList = GetDefault<UCommonValidatorsDeveloperSettings>()->HeavyValidatorClassAndChildIgnoreList;
		for (const TSubclassOf<UObject>& IgnoredChild : IgnoreChildrenList)
		{
			const UClass* const ParentClass = FBlueprintEditorUtils::FindFirstNativeClass(Blueprint->ParentClass);
			if (Blueprint->IsA(IgnoredChild) || ParentClass->IsChildOf(IgnoredChild))
			{
				return EDataValidationResult::NotValidated;
			}
		}
	}

	// Gather Specific Ref Classes to ignore
	TArray<TSubclassOf<UObject>, TInlineAllocator<4>> IgnoredClassList;
	for (auto& ClassToIgnoreEntry : GetDefault<UCommonValidatorsDeveloperSettings>()->HeavyValidatorClassSpecificClassIgnoreList)
	{
		const UClass* const IgnoredClass = ClassToIgnoreEntry.Key;
		
		const UClass* const ParentClass = FBlueprintEditorUtils::FindFirstNativeClass(Blueprint->ParentClass);
		if (Blueprint->IsA(IgnoredClass) || ParentClass->IsChildOf(IgnoredClass))
		{
			// These entries are valid for us
			IgnoredClassList.Append(ClassToIgnoreEntry.Value.ClassList);
		}
	}
	
	const bool bShouldError = GetDefault<UCommonValidatorsDeveloperSettings>()->bErrorHeavyReference;

	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<
		FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry* const AssetReg = &AssetRegistryModule.Get();
	IAssetManagerEditorModule* const EditorModule = &IAssetManagerEditorModule::Get();

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
			FAssetData FoundData = AssetReg->GetAssetByObjectPath(FSoftObjectPath(AssetPathString));

			if (FoundData.IsValid())
			{
				ThisAssetData = MoveTemp(FoundData);
			}
		}
		else
		{
			ThisAssetData = IAssetManagerEditorModule::CreateFakeAssetDataFromPrimaryAssetId(AssetPrimaryId);
		}

		// Ignore if this asset is in the ignore list
		bool bIsReferenceIgnored = false;
		for (const UClass* const IgnoreClass : IgnoredClassList)
		{
			// Needs to resolve BP class..
			auto ThisAssetClass = ThisAssetData.GetClass();
			if (ThisAssetClass->IsChildOf(IgnoreClass))
			{
				bIsReferenceIgnored = true;
			}
			
			//const UObject* GeneratedClassCDO = IsValid(AsBlueprint->GeneratedClass) ? AsBlueprint->GeneratedClass->GetDefaultObject() : nullptr;
			//const null* const AsType = Cast<null>(GeneratedClassCDO);
		}

		// Go for asset sizing
		if (ThisAssetData.IsValid() && !bIsReferenceIgnored)
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
				if (EditorModule->GetIntegerValueForCustomColumn(ThisAssetData, IAssetManagerEditorModule::ResourceSizeName, FoundSize))
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
	
	return EDataValidationResult::Valid;
}

#undef LOCTEXT_NAMESPACE
