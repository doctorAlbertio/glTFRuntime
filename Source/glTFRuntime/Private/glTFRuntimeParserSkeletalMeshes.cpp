// Copyright 2020, Roberto De Ioris.

#include "glTFRuntimeParser.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshVertexBuffer.h"
#if WITH_EDITOR
#include "IMeshBuilderModule.h"
#include "LODUtilities.h"
#include "MeshUtilities.h"
#endif
#include "Engine/SkeletalMeshSocket.h"
#include "glTFAnimBoneCompressionCodec.h"
#include "Animation/AnimCurveCompressionCodec_CompressedRichCurve.h"
#include "Model.h"
#include "Animation/MorphTarget.h"

void FglTFRuntimeParser::NormalizeSkeletonScale(FReferenceSkeleton& RefSkeleton)
{
	FReferenceSkeletonModifier Modifier = FReferenceSkeletonModifier(RefSkeleton, nullptr);
	NormalizeSkeletonBoneScale(Modifier, 0, FVector::OneVector);
}

void FglTFRuntimeParser::NormalizeSkeletonBoneScale(FReferenceSkeletonModifier& Modifier, const int32 BoneIndex, FVector BoneScale)
{
	TArray<FTransform> BonesTransforms = Modifier.GetReferenceSkeleton().GetRefBonePose();

	FTransform BoneTransform = BonesTransforms[BoneIndex];
	FVector ParentScale = BoneTransform.GetScale3D();
	BoneTransform.ScaleTranslation(BoneScale);
	BoneTransform.SetScale3D(FVector::OneVector);

	Modifier.UpdateRefPoseTransform(BoneIndex, BoneTransform);

	TArray<FMeshBoneInfo> MeshBoneInfos = Modifier.GetRefBoneInfo();
	for (int32 MeshBoneIndex = 0; MeshBoneIndex < MeshBoneInfos.Num(); MeshBoneIndex++)
	{
		FMeshBoneInfo& MeshBoneInfo = MeshBoneInfos[MeshBoneIndex];
		if (MeshBoneInfo.ParentIndex == BoneIndex)
		{
			NormalizeSkeletonBoneScale(Modifier, MeshBoneIndex, ParentScale * BoneScale);
		}
	}
}

USkeletalMesh* FglTFRuntimeParser::CreateSkeletalMeshFromLODs(TArray<FglTFRuntimeLOD> LODs, const int32 SkinIndex, const FglTFRuntimeSkeletalMeshConfig& SkeletalMeshConfig)
{
	USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Public);

	int32 NewSkinIndex = SkeletalMeshConfig.OverrideSkinIndex > INDEX_NONE ? SkeletalMeshConfig.OverrideSkinIndex : SkinIndex;

	TMap<int32, FName> MainBoneMap;
	if (!SkeletalMeshConfig.bIgnoreSkin && NewSkinIndex > INDEX_NONE)
	{
		TSharedPtr<FJsonObject>	JsonSkinObject = GetJsonObjectFromRootIndex("skins", NewSkinIndex);
		if (!JsonSkinObject)
		{
			AddError("CreateSkeletalMeshFromLODs()", "Unable to fill RefSkeleton.");
			return nullptr;
		}

		if (!FillReferenceSkeleton(JsonSkinObject.ToSharedRef(), SkeletalMesh->RefSkeleton, MainBoneMap, SkeletalMeshConfig.SkeletonConfig))
		{
			AddError("CreateSkeletalMeshFromLODs()", "Unable to fill RefSkeleton.");
			return nullptr;
		}
	}
	else
	{
		if (!FillFakeSkeleton(SkeletalMesh->RefSkeleton, MainBoneMap, SkeletalMeshConfig))
		{
			AddError("CreateSkeletalMeshFromLODs()", "Unable to fill fake RefSkeleton.");
			return nullptr;
		}
	}

	if (SkeletalMeshConfig.SkeletonConfig.bNormalizeSkeletonScale)
	{
		NormalizeSkeletonScale(SkeletalMesh->RefSkeleton);
	}

	if (SkeletalMeshConfig.Skeleton && SkeletalMeshConfig.bOverwriteRefSkeleton)
	{
		SkeletalMesh->RefSkeleton = SkeletalMeshConfig.Skeleton->GetReferenceSkeleton();
	}

	TMap<int32, int32> MainBonesCache;
	int32 MatIndex = 0;

	bool bHasNormals = false;

	SkeletalMesh->ResetLODInfo();

	FBox BoundingBox(EForceInit::ForceInitToZero);

#if WITH_EDITOR
	TArray<TSet<uint32>> MorphTargetModifiedPoints;
	TArray<FSkeletalMeshImportData> MorphTargetsData;
	TArray<FString> MorphTargetNames;

	FSkeletalMeshModel* ImportedResource = SkeletalMesh->GetImportedModel();
	ImportedResource->LODModels.Empty();

	for (FglTFRuntimeLOD& LOD : LODs)
	{
		TArray<SkeletalMeshImportData::FVertex> Wedges;
		TArray<SkeletalMeshImportData::FTriangle> Triangles;
		TArray<SkeletalMeshImportData::FRawBoneInfluence> Influences;

		TArray<FVector> Points;

		for (FglTFRuntimePrimitive& Primitive : LOD.Primitives)
		{
			int32 Base = Points.Num();
			Points.Append(Primitive.Positions);

			int32 TriangleIndex = 0;
			TSet<TPair<int32, int32>> InfluencesMap;

			TMap<int32, FName>& BoneMapInUse = Primitive.OverrideBoneMap.Num() > 0 ? Primitive.OverrideBoneMap : MainBoneMap;
			TMap<int32, int32>& BonesCacheInUse = Primitive.OverrideBoneMap.Num() > 0 ? Primitive.BonesCache : MainBonesCache;

			for (int32 i = 0; i < Primitive.Indices.Num(); i++)
			{
				int32 PrimitiveIndex = Primitive.Indices[i];

				SkeletalMeshImportData::FVertex Wedge;
				Wedge.VertexIndex = Base + PrimitiveIndex;

				for (int32 UVIndex = 0; UVIndex < Primitive.UVs.Num(); UVIndex++)
				{
					Wedge.UVs[UVIndex] = Primitive.UVs[UVIndex][PrimitiveIndex];
				}

				int32 WedgeIndex = Wedges.Add(Wedge);

				for (int32 JointsIndex = 0; JointsIndex < Primitive.Joints.Num(); JointsIndex++)
				{
					FglTFRuntimeUInt16Vector4 Joints = Primitive.Joints[JointsIndex][PrimitiveIndex];
					FVector4 Weights = Primitive.Weights[JointsIndex][PrimitiveIndex];
					// 4 bones for each joints list
					for (int32 JointPartIndex = 0; JointPartIndex < 4; JointPartIndex++)
					{
						if (BoneMapInUse.Contains(Joints[JointPartIndex]))
						{
							SkeletalMeshImportData::FRawBoneInfluence Influence;
							Influence.VertexIndex = Wedge.VertexIndex;
							int32 BoneIndex = INDEX_NONE;
							if (BonesCacheInUse.Contains(Joints[JointPartIndex]))
							{
								BoneIndex = BonesCacheInUse[Joints[JointPartIndex]];
							}
							else
							{
								BoneIndex = SkeletalMesh->RefSkeleton.FindBoneIndex(BoneMapInUse[Joints[JointPartIndex]]);
								BonesCacheInUse.Add(Joints[JointPartIndex], BoneIndex);
							}
							Influence.BoneIndex = BoneIndex;
							Influence.Weight = Weights[JointPartIndex];
							TPair<int32, int32> InfluenceKey = TPair<int32, int32>(Influence.VertexIndex, Influence.BoneIndex);
							// do not waste cpu time processing zero influences
							if (!FMath::IsNearlyZero(Influence.Weight, KINDA_SMALL_NUMBER) && !InfluencesMap.Contains(InfluenceKey))
							{
								Influences.Add(Influence);
								InfluencesMap.Add(InfluenceKey);
							}
						}
						else
						{
							AddError("LoadSkeletalMesh_Internal()", FString::Printf(TEXT("Unable to find map for bone %u"), Joints[JointPartIndex]));
							return nullptr;
						}
					}
				}

				TriangleIndex++;
				if (TriangleIndex == 3)
				{
					SkeletalMeshImportData::FTriangle Triangle;

					Triangle.WedgeIndex[0] = WedgeIndex - 2;
					Triangle.WedgeIndex[1] = WedgeIndex - 1;
					Triangle.WedgeIndex[2] = WedgeIndex;

					if (Primitive.Normals.Num() > 0)
					{
						Triangle.TangentZ[0] = Primitive.Normals[Primitive.Indices[i - 2]];
						Triangle.TangentZ[1] = Primitive.Normals[Primitive.Indices[i - 1]];
						Triangle.TangentZ[2] = Primitive.Normals[Primitive.Indices[i]];
						bHasNormals = true;
					}

					if (Primitive.Tangents.Num() > 0)
					{
						Triangle.TangentX[0] = Primitive.Tangents[Primitive.Indices[i - 2]];
						Triangle.TangentX[1] = Primitive.Tangents[Primitive.Indices[i - 1]];
						Triangle.TangentX[2] = Primitive.Tangents[Primitive.Indices[i]];
					}

					Triangle.MatIndex = MatIndex;

					Triangles.Add(Triangle);
					TriangleIndex = 0;
				}
			}

			MatIndex++;
		}

		FSkeletalMeshImportData ImportData;

		TArray<int32> PointToRawMap;
		for (int32 PointIndex = 0; PointIndex < Points.Num(); PointIndex++)
		{
			// update boundingbox
			BoundingBox += Points[PointIndex] * SkeletalMeshConfig.BoundsScale;
			PointToRawMap.Add(PointIndex);
		}

		if (SkeletalMeshConfig.bIgnoreSkin || NewSkinIndex <= INDEX_NONE)
		{
			Influences.Empty();
			TSet<int32> VertexIndexHistory;
			for (int32 WedgeIndex = 0; WedgeIndex < Wedges.Num(); WedgeIndex++)
			{
				if (VertexIndexHistory.Contains(Wedges[WedgeIndex].VertexIndex))
				{
					continue;
				}
				SkeletalMeshImportData::FRawBoneInfluence Influence;
				Influence.VertexIndex = Wedges[WedgeIndex].VertexIndex;
				Influence.BoneIndex = 0;
				Influence.Weight = 1;
				Influences.Add(Influence);
				VertexIndexHistory.Add(Influence.VertexIndex);
			}
		}

#if ENGINE_MINOR_VERSION > 25
		FLODUtilities::ProcessImportMeshInfluences(Wedges.Num(), Influences, FString::Printf(TEXT("LOD_%d"), ImportedResource->LODModels.Num()));
#else
		FLODUtilities::ProcessImportMeshInfluences(Wedges.Num(), Influences);
#endif

		ImportData.bHasNormals = bHasNormals;
		ImportData.bHasVertexColors = false;
		ImportData.bHasTangents = false;
		ImportData.Faces = Triangles;
		ImportData.Points = Points;
		ImportData.PointToRawMap = PointToRawMap;
		ImportData.NumTexCoords = 1;
		ImportData.Wedges = Wedges;
		ImportData.Influences = Influences;

		int32 MorphTargetIndex = 0;
		int32 PointsBase = 0;
		for (FglTFRuntimePrimitive& Primitive : LOD.Primitives)
		{
			for (FglTFRuntimeMorphTarget& MorphTarget : Primitive.MorphTargets)
			{
				TSet<uint32> MorphTargetPoints;
				TArray<FVector> MorphTargetPositions;
				for (uint32 PointIndex = 0; PointIndex < (uint32)Primitive.Positions.Num(); PointIndex++)
				{
					MorphTargetPoints.Add(PointsBase + PointIndex);
					MorphTargetPositions.Add(Primitive.Positions[PointIndex] + MorphTarget.Positions[PointIndex]);
				}
				MorphTargetModifiedPoints.Add(MorphTargetPoints);
				FSkeletalMeshImportData MorphTargetImportData;
				ImportData.CopyDataNeedByMorphTargetImport(MorphTargetImportData);
				MorphTargetImportData.Points = MorphTargetPositions;
				MorphTargetsData.Add(MorphTargetImportData);
				const FString MorphTargetName = FString::Printf(TEXT("MorphTarget_%d"), MorphTargetIndex++);
				MorphTargetNames.Add(MorphTargetName);
			}
			PointsBase += Primitive.Positions.Num();
		}

		ImportData.MorphTargetModifiedPoints = MorphTargetModifiedPoints;
		ImportData.MorphTargets = MorphTargetsData;
		ImportData.MorphTargetNames = MorphTargetNames;


		int32 LODIndex = ImportedResource->LODModels.Add(new FSkeletalMeshLODModel());
		FSkeletalMeshLODModel& LODModel = ImportedResource->LODModels[LODIndex];

		SkeletalMesh->SaveLODImportedData(LODIndex, ImportData);
#else

	SkeletalMesh->AllocateResourceForRendering();

	for (FglTFRuntimeLOD& LOD : LODs)
	{
		FSkeletalMeshLODRenderData* LodRenderData = new FSkeletalMeshLODRenderData();
		int32 LODIndex = SkeletalMesh->GetResourceForRendering()->LODRenderData.Add(LodRenderData);

		LodRenderData->RenderSections.SetNumUninitialized(LOD.Primitives.Num());

		int32 NumIndices = 0;
		for (int32 PrimitiveIndex = 0; PrimitiveIndex < LOD.Primitives.Num(); PrimitiveIndex++)
		{
			NumIndices += LOD.Primitives[PrimitiveIndex].Indices.Num();
		}

		LodRenderData->StaticVertexBuffers.PositionVertexBuffer.Init(NumIndices);
		LodRenderData->StaticVertexBuffers.StaticMeshVertexBuffer.Init(NumIndices, 1);

		int32 NumBones = SkeletalMesh->RefSkeleton.GetNum();

		for (int32 BoneIndex = 0; BoneIndex < NumBones; BoneIndex++)
		{
			LodRenderData->RequiredBones.Add(BoneIndex);
			LodRenderData->ActiveBoneIndices.Add(BoneIndex);
		}

		TArray<FSkinWeightInfo> InWeights;
		InWeights.AddUninitialized(NumIndices);

		int32 TotalVertexIndex = 0;

		TArray<FVector> Points;

		for (int32 PrimitiveIndex = 0; PrimitiveIndex < LOD.Primitives.Num(); PrimitiveIndex++)
		{
			FglTFRuntimePrimitive& Primitive = LOD.Primitives[PrimitiveIndex];

			int32 Base = Points.Num();
			for (FVector& Point : Primitive.Positions)
			{
				BoundingBox += Point * SkeletalMeshConfig.BoundsScale;
				Points.Add(Point);
			}

			new(&LodRenderData->RenderSections[PrimitiveIndex]) FSkelMeshRenderSection();
			FSkelMeshRenderSection& MeshSection = LodRenderData->RenderSections[PrimitiveIndex];

			MeshSection.MaterialIndex = PrimitiveIndex;
			MeshSection.BaseIndex = TotalVertexIndex;
			MeshSection.NumTriangles = Primitive.Indices.Num() / 3;
			MeshSection.BaseVertexIndex = Base;
			MeshSection.MaxBoneInfluences = 4;

			MeshSection.NumVertices = Primitive.Positions.Num();

			TMap<int32, TArray<int32>> OverlappingVertices;
			MeshSection.DuplicatedVerticesBuffer.Init(MeshSection.NumVertices, OverlappingVertices);

			for (int32 VertexIndex = 0; VertexIndex < Primitive.Indices.Num(); VertexIndex++)
			{
				int32 Index = Primitive.Indices[VertexIndex];
				FModelVertex ModelVertex;
				ModelVertex.Position = Primitive.Positions[Index];
				ModelVertex.TangentX = FVector::ZeroVector;
				ModelVertex.TangentZ = FVector::ZeroVector;
				if (Index < Primitive.Normals.Num())
				{
					ModelVertex.TangentZ = Primitive.Normals[Index];
					bHasNormals = true;
				}
				if (Index < Primitive.Tangents.Num())
				{
					ModelVertex.TangentX = Primitive.Tangents[Index];
				}
				if (Primitive.UVs.Num() > 0 && Index < Primitive.UVs[0].Num())
				{
					ModelVertex.TexCoord = Primitive.UVs[0][Index];
				}
				else
				{
					ModelVertex.TexCoord = FVector2D::ZeroVector;
				}

				LodRenderData->StaticVertexBuffers.PositionVertexBuffer.VertexPosition(TotalVertexIndex) = ModelVertex.Position;
				LodRenderData->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(TotalVertexIndex, ModelVertex.TangentX, ModelVertex.GetTangentY(), ModelVertex.TangentZ);
				LodRenderData->StaticVertexBuffers.StaticMeshVertexBuffer.SetVertexUV(TotalVertexIndex, 0, ModelVertex.TexCoord);

				TMap<int32, FName>& BoneMapInUse = Primitive.OverrideBoneMap.Num() > 0 ? Primitive.OverrideBoneMap : MainBoneMap;
				TMap<int32, int32>& BonesCacheInUse = Primitive.OverrideBoneMap.Num() > 0 ? Primitive.BonesCache : MainBonesCache;

				if (!SkeletalMeshConfig.bIgnoreSkin && NewSkinIndex > INDEX_NONE)
				{
					for (int32 JointsIndex = 0; JointsIndex < Primitive.Joints.Num(); JointsIndex++)
					{
						FglTFRuntimeUInt16Vector4 Joints = Primitive.Joints[JointsIndex][Index];
						FVector4 Weights = Primitive.Weights[JointsIndex][Index];
						for (int32 j = 0; j < 4; j++)
						{
							if (BoneMapInUse.Contains(Joints[j]))
							{
								int32 BoneIndex = INDEX_NONE;
								if (BonesCacheInUse.Contains(Joints[j]))
								{
									BoneIndex = BonesCacheInUse[Joints[j]];
								}
								else
								{
									BoneIndex = SkeletalMesh->RefSkeleton.FindBoneIndex(BoneMapInUse[Joints[j]]);
									BonesCacheInUse.Add(Joints[j], BoneIndex);
								}

								uint8 QuantizedWeight = FMath::Clamp((uint8)(Weights[j] * ((double)0xFF)), (uint8)0x00, (uint8)0xFF);

								InWeights[TotalVertexIndex].InfluenceWeights[j] = QuantizedWeight;
								InWeights[TotalVertexIndex].InfluenceBones[j] = BoneIndex;
							}
							else
							{
								AddError("LoadSkeletalMesh_Internal()", FString::Printf(TEXT("Unable to find map for bone %u"), Joints[j]));
								return nullptr;
							}
						}
					}
				}
				else
				{
					for (int32 j = 0; j < 4; j++)
					{
						InWeights[TotalVertexIndex].InfluenceWeights[j] = j == 0 ? 0xFF : 0;
						InWeights[TotalVertexIndex].InfluenceBones[j] = 0;
					}
				}

				TotalVertexIndex++;
			}

			for (int32 BoneIndex = 0; BoneIndex < NumBones; BoneIndex++)
			{
				MeshSection.BoneMap.Add(BoneIndex);
			}
		}

		LodRenderData->SkinWeightVertexBuffer.SetMaxBoneInfluences(4);
		LodRenderData->SkinWeightVertexBuffer = InWeights;
		LodRenderData->MultiSizeIndexContainer.CreateIndexBuffer(sizeof(uint32_t));

		for (int32 Index = 0; Index < NumIndices; Index++)
		{
			LodRenderData->MultiSizeIndexContainer.GetIndexBuffer()->AddItem(Index);
		}

		bool bHasMorphTargets = false;
		int32 MorphTargetIndex = 0;
		int32 BaseIndex = 0;
		for (int32 PrimitiveIndex = 0; PrimitiveIndex < LOD.Primitives.Num(); PrimitiveIndex++)
		{
			FglTFRuntimePrimitive& Primitive = LOD.Primitives[PrimitiveIndex];

			for (FglTFRuntimeMorphTarget& MorphTargetData : Primitive.MorphTargets)
			{
				FMorphTargetLODModel MorphTargetLODModel;
				MorphTargetLODModel.NumBaseMeshVerts = Primitive.Indices.Num();
				MorphTargetLODModel.SectionIndices.Add(PrimitiveIndex);

				const FString MorphTargetName = FString::Printf(TEXT("MorphTarget_%d"), MorphTargetIndex++);
				UMorphTarget* MorphTarget = NewObject<UMorphTarget>(SkeletalMesh, *MorphTargetName, RF_Public);
				for (int32 Index = 0; Index < Primitive.Indices.Num(); Index++)
				{
					FMorphTargetDelta Delta;
					int32 VertexIndex = Primitive.Indices[Index];
					if (VertexIndex < MorphTargetData.Positions.Num())
					{
						Delta.PositionDelta = MorphTargetData.Positions[VertexIndex];
					}
					else
					{
						Delta.PositionDelta = FVector::ZeroVector;
					}
					Delta.SourceIdx = BaseIndex + Index;
					Delta.TangentZDelta = FVector(0, 0, 0);
					MorphTargetLODModel.Vertices.Add(Delta);
				}
				MorphTarget->MorphLODModels.Add(MorphTargetLODModel);
				SkeletalMesh->RegisterMorphTarget(MorphTarget, false);
				bHasMorphTargets = true;
			}
			BaseIndex += Primitive.Indices.Num();
		}

		if (bHasMorphTargets)
		{
			SkeletalMesh->InitMorphTargetsAndRebuildRenderData();
		}
#endif
		// LOD tuning

		FSkeletalMeshLODInfo& LODInfo = SkeletalMesh->AddLODInfo();
		LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
		LODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
		LODInfo.ReductionSettings.MaxDeviationPercentage = 0.0f;
		LODInfo.BuildSettings.bRecomputeNormals = !bHasNormals;
		LODInfo.LODHysteresis = 0.02f;
		if (SkeletalMeshConfig.LODScreenSize.Contains(LODIndex))
		{
			LODInfo.ScreenSize = SkeletalMeshConfig.LODScreenSize[LODIndex];
		}

		for (MatIndex = 0; MatIndex < LOD.Primitives.Num(); MatIndex++)
		{
			LODInfo.LODMaterialMap.Add(MatIndex);
			int32 NewMatIndex = SkeletalMesh->Materials.Add(LOD.Primitives[MatIndex].Material);
			SkeletalMesh->Materials[NewMatIndex].UVChannelData.bInitialized = true;
			SkeletalMesh->Materials[NewMatIndex].MaterialSlotName = FName(FString::Printf(TEXT("LOD_%d_Section_%d"), LODIndex, MatIndex));
		}

#if WITH_EDITOR
		IMeshBuilderModule& MeshBuilderModule = IMeshBuilderModule::GetForRunningPlatform();
		if (!MeshBuilderModule.BuildSkeletalMesh(SkeletalMesh, LODIndex, false))
		{
			return nullptr;
		}

		SkeletalMesh->Build();
#endif
	}

	SkeletalMesh->CalculateInvRefMatrices();

	if (SkeletalMeshConfig.bShiftBoundsByRootBone)
	{
		FVector RootBone = SkeletalMesh->RefSkeleton.GetRefBonePose()[0].GetLocation();
		BoundingBox = BoundingBox.ShiftBy(RootBone);
	}

	SkeletalMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));

	SkeletalMesh->bHasVertexColors = false;
#if WITH_EDITOR
	SkeletalMesh->VertexColorGuid = SkeletalMesh->bHasVertexColors ? FGuid::NewGuid() : FGuid();
#endif

	if (SkeletalMeshConfig.Skeleton)
	{
		SkeletalMesh->Skeleton = SkeletalMeshConfig.Skeleton;
	}
	else
	{
		if (CanReadFromCache(SkeletalMeshConfig.SkeletonConfig.CacheMode) && SkeletonsCache.Contains(NewSkinIndex))
		{
			SkeletalMesh->Skeleton = SkeletonsCache[NewSkinIndex];
		}
		else
		{
			SkeletalMesh->Skeleton = NewObject<USkeleton>(GetTransientPackage(), NAME_None, RF_Public);
			SkeletalMesh->Skeleton->MergeAllBonesToBoneTree(SkeletalMesh);
			if (CanWriteToCache(SkeletalMeshConfig.SkeletonConfig.CacheMode))
			{
				SkeletonsCache.Add(NewSkinIndex, SkeletalMesh->Skeleton);
			}
			SkeletalMesh->Skeleton->SetPreviewMesh(SkeletalMesh);
		}

		for (const TPair<FString, FglTFRuntimeSocket>& Pair : SkeletalMeshConfig.SkeletonConfig.Sockets)
		{
			USkeletalMeshSocket* SkeletalSocket = NewObject<USkeletalMeshSocket>(SkeletalMesh->Skeleton);
			SkeletalSocket->SocketName = FName(Pair.Key);
			SkeletalSocket->BoneName = FName(Pair.Value.BoneName);
			SkeletalSocket->RelativeLocation = Pair.Value.Transform.GetLocation();
			SkeletalSocket->RelativeRotation = Pair.Value.Transform.GetRotation().Rotator();
			SkeletalSocket->RelativeScale = Pair.Value.Transform.GetScale3D();
			SkeletalMesh->Skeleton->Sockets.Add(SkeletalSocket);
		}
	}

#if !WITH_EDITOR
	SkeletalMesh->PostLoad();
#endif

	if (OnSkeletalMeshCreated.IsBound())
	{
		OnSkeletalMeshCreated.Broadcast(SkeletalMesh);
	}

	return SkeletalMesh;
}

USkeletalMesh* FglTFRuntimeParser::LoadSkeletalMesh(const int32 MeshIndex, const int32 SkinIndex, const FglTFRuntimeSkeletalMeshConfig & SkeletalMeshConfig)
{

	// first check cache
	if (CanReadFromCache(SkeletalMeshConfig.CacheMode) && SkeletalMeshesCache.Contains(MeshIndex))
	{
		return SkeletalMeshesCache[MeshIndex];
	}

	TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
	if (!JsonMeshObject)
	{
		AddError("LoadSkeletalMesh()", FString::Printf(TEXT("Unable to find Mesh with index %d"), MeshIndex));
		return nullptr;
	}

	// get primitives
	const TArray<TSharedPtr<FJsonValue>>* JsonPrimitives;
	if (!JsonMeshObject->TryGetArrayField("primitives", JsonPrimitives))
	{
		AddError("LoadSkeletalMesh", "No primitives defined in the asset.");
		return nullptr;
	}

	TArray<FglTFRuntimePrimitive> Primitives;
	if (!LoadPrimitives(JsonPrimitives, Primitives, SkeletalMeshConfig.MaterialsConfig))
	{
		return nullptr;
	}

	TArray<FglTFRuntimeLOD> LODs;
	FglTFRuntimeLOD LOD0;
	LOD0.Primitives = Primitives;
	LODs.Add(LOD0);

	USkeletalMesh* SkeletalMesh = CreateSkeletalMeshFromLODs(LODs, SkinIndex, SkeletalMeshConfig);
	if (!SkeletalMesh)
	{
		AddError("LoadSkeletalMesh()", "Unable to load SkeletalMesh.");
		return nullptr;
	}

	if (CanWriteToCache(SkeletalMeshConfig.CacheMode))
	{
		SkeletalMeshesCache.Add(MeshIndex, SkeletalMesh);
	}

	return SkeletalMesh;
}

USkeletalMesh* FglTFRuntimeParser::LoadSkeletalMeshLODs(const TArray<int32> MeshIndices, const int32 SkinIndex, const FglTFRuntimeSkeletalMeshConfig & SkeletalMeshConfig)
{
	TArray<FglTFRuntimeLOD> LODs;

	for (const int32 MeshIndex : MeshIndices)
	{
		TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", MeshIndex);
		if (!JsonMeshObject)
		{
			AddError("LoadSkeletalMesh()", FString::Printf(TEXT("Unable to find Mesh with index %d"), MeshIndex));
			return nullptr;
		}

		// get primitives
		const TArray<TSharedPtr<FJsonValue>>* JsonPrimitives;
		if (!JsonMeshObject->TryGetArrayField("primitives", JsonPrimitives))
		{
			AddError("LoadSkeletalMesh", "No primitives defined in the asset.");
			return nullptr;
		}

		TArray<FglTFRuntimePrimitive> Primitives;
		if (!LoadPrimitives(JsonPrimitives, Primitives, SkeletalMeshConfig.MaterialsConfig))
		{
			return nullptr;
		}

		FglTFRuntimeLOD LOD;
		LOD.Primitives = Primitives;
		LODs.Add(LOD);
	}

	return CreateSkeletalMeshFromLODs(LODs, SkinIndex, SkeletalMeshConfig);
}

USkeletalMesh* FglTFRuntimeParser::LoadSkeletalMeshRecursive(const FString & NodeName, const int32 SkinIndex, const FglTFRuntimeSkeletalMeshConfig SkeletalMeshConfig)
{
	FglTFRuntimeNode Node;
	if (!LoadNodeByName(NodeName, Node))
	{
		AddError("LoadSkeletalMeshRecursive()", FString::Printf(TEXT("Unable to find Node \"%s\""), *NodeName));
		return nullptr;
	}

	TArray<FglTFRuntimeNode> Nodes;
	if (!LoadNodesRecursive(Node.Index, Nodes))
	{
		AddError("LoadSkeletalMeshRecursive()", FString::Printf(TEXT("Unable to build Node Tree from \"%s\""), *NodeName));
		return nullptr;
	}

	int32 NewSkinIndex = SkinIndex;

	if (NewSkinIndex <= INDEX_NONE)
	{
		// first search for skinning
		for (FglTFRuntimeNode& ChildNode : Nodes)
		{
			if (ChildNode.SkinIndex > INDEX_NONE)
			{
				NewSkinIndex = ChildNode.SkinIndex;
				break;
			}
		}

		if (NewSkinIndex <= INDEX_NONE)
		{
			AddError("LoadSkeletalMeshRecursive()", "Unable to find a valid Skin");
			return nullptr;
		}
	}

	TArray<FglTFRuntimePrimitive> Primitives;

	// now search for all meshes (will be all merged in the same primitives list)
	for (FglTFRuntimeNode& ChildNode : Nodes)
	{
		if (ChildNode.MeshIndex != INDEX_NONE)
		{
			TSharedPtr<FJsonObject> JsonMeshObject = GetJsonObjectFromRootIndex("meshes", ChildNode.MeshIndex);
			if (!JsonMeshObject)
			{
				AddError("LoadSkeletalMeshRecursive()", FString::Printf(TEXT("Unable to find Mesh with index %d"), ChildNode.MeshIndex));
				return nullptr;
			}

			// get primitives
			const TArray<TSharedPtr<FJsonValue>>* JsonPrimitives;
			if (!JsonMeshObject->TryGetArrayField("primitives", JsonPrimitives))
			{
				AddError("LoadSkeletalMeshRecursive()", "No primitives defined in the asset.");
				return nullptr;
			}

			// keep track of primitives
			int32 PrimitiveFirstIndex = Primitives.Num();

			if (!LoadPrimitives(JsonPrimitives, Primitives, SkeletalMeshConfig.MaterialsConfig))
			{
				return nullptr;
			}

			// if the SkinIndex is different from the selected one,
			// build an override bone map
			if (ChildNode.SkinIndex > INDEX_NONE && ChildNode.SkinIndex != NewSkinIndex)
			{
				TSharedPtr<FJsonObject> JsonSkinObject = GetJsonObjectFromRootIndex("skins", ChildNode.SkinIndex);
				if (!JsonSkinObject)
				{
					AddError("LoadSkeletalMeshRecursive()", FString::Printf(TEXT("Unable to fill skin %d"), ChildNode.SkinIndex));
					return nullptr;
				}

				TMap<int32, FName> BoneMap;

				FReferenceSkeleton FakeRefSkeleton;
				if (!FillReferenceSkeleton(JsonSkinObject.ToSharedRef(), FakeRefSkeleton, BoneMap, SkeletalMeshConfig.SkeletonConfig))
				{
					AddError("LoadSkeletalMeshRecursive()", "Unable to fill RefSkeleton.");
					return nullptr;
				}

				// apply overrides
				for (int32 PrimitiveIndex = PrimitiveFirstIndex; PrimitiveIndex < Primitives.Num(); PrimitiveIndex++)
				{
					FglTFRuntimePrimitive& Primitive = Primitives[PrimitiveIndex];
					Primitive.OverrideBoneMap = BoneMap;
				}
			}
		}
	}

	TArray<FglTFRuntimeLOD> LODs;
	FglTFRuntimeLOD LOD0;
	LOD0.Primitives = Primitives;
	LODs.Add(LOD0);

	return CreateSkeletalMeshFromLODs(LODs, NewSkinIndex, SkeletalMeshConfig);
}

UAnimSequence* FglTFRuntimeParser::LoadSkeletalAnimationByName(USkeletalMesh * SkeletalMesh, const FString AnimationName, const FglTFRuntimeSkeletalAnimationConfig & AnimationConfig)
{
	if (!SkeletalMesh)
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* JsonAnimations;
	if (!Root->TryGetArrayField("animations", JsonAnimations))
	{
		AddError("LoadSkeletalAnimationByName()", "No animations defined in the asset.");
		return nullptr;
	}

	for (int32 AnimationIndex = 0; AnimationIndex < JsonAnimations->Num(); AnimationIndex++)
	{
		TSharedPtr<FJsonObject> JsonAnimationObject = (*JsonAnimations)[AnimationIndex]->AsObject();
		if (!JsonAnimationObject)
		{
			return nullptr;
		}

		FString JsonAnimationName;
		if (JsonAnimationObject->TryGetStringField("name", JsonAnimationName))
		{
			if (JsonAnimationName == AnimationName)
			{
				return LoadSkeletalAnimation(SkeletalMesh, AnimationIndex, AnimationConfig);
			}
		}
	}

	return nullptr;
}

UAnimSequence* FglTFRuntimeParser::LoadNodeSkeletalAnimation(USkeletalMesh * SkeletalMesh, const int32 NodeIndex, const FglTFRuntimeSkeletalAnimationConfig SkeletalAnimationConfig)
{

	if (!SkeletalMesh)
	{
		return nullptr;
	}

	FglTFRuntimeNode Node;
	if (!LoadNode(NodeIndex, Node))
	{
		return nullptr;
	}

	if (Node.SkinIndex <= INDEX_NONE)
	{
		AddError("LoadNodeSkeletalAnimation()", FString::Printf(TEXT("No skin defined for node %d"), NodeIndex));
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonSkinObject = GetJsonObjectFromRootIndex("skins", Node.SkinIndex);
	if (!JsonSkinObject)
	{
		AddError("LoadNodeSkeletalAnimation()", "No skins defined in the asset");
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* JsonJoints;
	if (!JsonSkinObject->TryGetArrayField("joints", JsonJoints))
	{
		AddError("LoadNodeSkeletalAnimation()", "No joints defined in the skin");
		return nullptr;
	}

	TArray<int32> Joints;
	for (TSharedPtr<FJsonValue> JsonJoint : (*JsonJoints))
	{
		int64 JointIndex;
		if (!JsonJoint->TryGetNumber(JointIndex))
		{
			return nullptr;
		}
		Joints.Add(JointIndex);
	}

	const TArray<TSharedPtr<FJsonValue>>* JsonAnimations;
	if (!Root->TryGetArrayField("animations", JsonAnimations))
	{
		AddError("LoadNodeSkeletalAnimation()", "No animations defined in the asset");
		return nullptr;
	}

	for (int32 JsonAnimationIndex = 0; JsonAnimationIndex < JsonAnimations->Num(); JsonAnimationIndex++)
	{
		TSharedPtr<FJsonObject> JsonAnimationObject = (*JsonAnimations)[JsonAnimationIndex]->AsObject();
		if (!JsonAnimationObject)
			return nullptr;
		float Duration;
		TMap<FString, FRawAnimSequenceTrack> Tracks;
		bool bAnimationFound = false;
		if (!LoadSkeletalAnimation_Internal(JsonAnimationObject.ToSharedRef(), Tracks, Duration, [Joints, &bAnimationFound](const FglTFRuntimeNode& Node) -> bool
		{
			bAnimationFound = Joints.Contains(Node.Index);
			return bAnimationFound;
		}))
		{
			return nullptr;
		}

		if (bAnimationFound)
		{
			// this is very inefficient as we parse the tracks twice
			// TODO: refactor it
			return LoadSkeletalAnimation(SkeletalMesh, JsonAnimationIndex, SkeletalAnimationConfig);
		}
	}

	return nullptr;
}


UAnimSequence* FglTFRuntimeParser::LoadSkeletalAnimation(USkeletalMesh * SkeletalMesh, const int32 AnimationIndex, const FglTFRuntimeSkeletalAnimationConfig & AnimationConfig)
{
	if (!SkeletalMesh)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonAnimationObject = GetJsonObjectFromRootIndex("animations", AnimationIndex);
	if (!JsonAnimationObject)
	{
		AddError("LoadNodeSkeletalAnimation()", FString::Printf(TEXT("Unable to find animation %d"), AnimationIndex));
		return nullptr;
	}

	float Duration;
	TMap<FString, FRawAnimSequenceTrack> Tracks;
	if (!LoadSkeletalAnimation_Internal(JsonAnimationObject.ToSharedRef(), Tracks, Duration, [](const FglTFRuntimeNode& Node) -> bool { return true; }))
	{
		return nullptr;
	}

	int32 NumFrames = Duration * 30;
	UAnimSequence* AnimSequence = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Public);
	AnimSequence->SetSkeleton(SkeletalMesh->Skeleton);
	AnimSequence->SetPreviewMesh(SkeletalMesh);
	AnimSequence->SetRawNumberOfFrame(NumFrames);
	AnimSequence->SequenceLength = Duration;
	AnimSequence->bEnableRootMotion = AnimationConfig.bRootMotion;

	const TArray<FTransform> BonesPoses = AnimSequence->GetSkeleton()->GetReferenceSkeleton().GetRefBonePose();

#if !WITH_EDITOR
	UglTFAnimBoneCompressionCodec* CompressionCodec = NewObject<UglTFAnimBoneCompressionCodec>();
	CompressionCodec->Tracks.AddDefaulted(BonesPoses.Num());
	AnimSequence->CompressedData.CompressedTrackToSkeletonMapTable.AddDefaulted(BonesPoses.Num());
	for (int32 BoneIndex = 0; BoneIndex < BonesPoses.Num(); BoneIndex++)
	{
		AnimSequence->CompressedData.CompressedTrackToSkeletonMapTable[BoneIndex] = BoneIndex;
		for (int32 FrameIndex = 0; FrameIndex < NumFrames; FrameIndex++)
		{
			CompressionCodec->Tracks[BoneIndex].PosKeys.Add(BonesPoses[BoneIndex].GetLocation());
			CompressionCodec->Tracks[BoneIndex].RotKeys.Add(BonesPoses[BoneIndex].GetRotation());
			CompressionCodec->Tracks[BoneIndex].ScaleKeys.Add(BonesPoses[BoneIndex].GetScale3D());
		}
}
#endif

	bool bHasTracks = false;
	for (TPair<FString, FRawAnimSequenceTrack>& Pair : Tracks)
	{
		FName BoneName = FName(Pair.Key);
		int32 BoneIndex = AnimSequence->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			AddError("LoadSkeletalAnimation()", FString::Printf(TEXT("Unable to find bone %s"), *Pair.Key));
			continue;
		}

		// sanitize curves

		// positions
		if (Pair.Value.PosKeys.Num() == 0)
		{
			for (int32 FrameIndex = 0; FrameIndex < NumFrames; FrameIndex++)
			{
				Pair.Value.PosKeys.Add(BonesPoses[BoneIndex].GetLocation());
			}
		}
		else if (Pair.Value.PosKeys.Num() < NumFrames)
		{
			FVector LastValidPosition = Pair.Value.PosKeys.Last();
			int32 FirstNewFrame = Pair.Value.PosKeys.Num();
			for (int32 FrameIndex = FirstNewFrame; FrameIndex < NumFrames; FrameIndex++)
			{
				Pair.Value.PosKeys.Add(LastValidPosition);
			}
		}
		else
		{
			Pair.Value.PosKeys.RemoveAt(NumFrames, Pair.Value.PosKeys.Num() - NumFrames, true);
		}

		// rotations
		if (Pair.Value.RotKeys.Num() == 0)
		{
			for (int32 FrameIndex = 0; FrameIndex < NumFrames; FrameIndex++)
			{
				Pair.Value.RotKeys.Add(BonesPoses[BoneIndex].GetRotation());
			}
		}
		else if (Pair.Value.RotKeys.Num() < NumFrames)
		{
			FQuat LastValidRotation = Pair.Value.RotKeys.Last();
			int32 FirstNewFrame = Pair.Value.RotKeys.Num();
			for (int32 FrameIndex = FirstNewFrame; FrameIndex < NumFrames; FrameIndex++)
			{
				Pair.Value.RotKeys.Add(LastValidRotation);
			}
		}
		else
		{
			Pair.Value.RotKeys.RemoveAt(NumFrames, Pair.Value.RotKeys.Num() - NumFrames, true);
		}

		if (Pair.Value.ScaleKeys.Num() == 0)
		{
			for (int32 FrameIndex = 0; FrameIndex < NumFrames; FrameIndex++)
			{
				Pair.Value.ScaleKeys.Add(BonesPoses[BoneIndex].GetScale3D());
			}
		}
		else if (Pair.Value.ScaleKeys.Num() < NumFrames)
		{
			FVector LastValidScale = Pair.Value.ScaleKeys.Last();
			int32 FirstNewFrame = Pair.Value.ScaleKeys.Num();
			for (int32 FrameIndex = FirstNewFrame; FrameIndex < NumFrames; FrameIndex++)
			{
				Pair.Value.ScaleKeys.Add(LastValidScale);
			}
		}
		else
		{
			Pair.Value.ScaleKeys.RemoveAt(NumFrames, Pair.Value.ScaleKeys.Num() - NumFrames, true);
		}


		if (BoneIndex == 0)
		{
			if (AnimationConfig.RootNodeIndex > INDEX_NONE)
			{
				FglTFRuntimeNode AnimRootNode;
				if (!LoadNode(AnimationConfig.RootNodeIndex, AnimRootNode))
				{
					return nullptr;
				}

				for (int32 FrameIndex = 0; FrameIndex < Pair.Value.RotKeys.Num(); FrameIndex++)
				{
					FVector Pos = Pair.Value.PosKeys[FrameIndex];
					FQuat Quat = Pair.Value.RotKeys[FrameIndex];
					FVector Scale = Pair.Value.ScaleKeys[FrameIndex];

					FTransform FrameTransform = FTransform(Quat, Pos, Scale) * AnimRootNode.Transform;

					Pair.Value.PosKeys[FrameIndex] = FrameTransform.GetLocation();
					Pair.Value.RotKeys[FrameIndex] = FrameTransform.GetRotation();
					Pair.Value.ScaleKeys[FrameIndex] = FrameTransform.GetScale3D();
				}
			}

			if (AnimationConfig.bRemoveRootMotion)
			{
				for (int32 FrameIndex = 0; FrameIndex < Pair.Value.RotKeys.Num(); FrameIndex++)
				{
					Pair.Value.PosKeys[FrameIndex] = Pair.Value.PosKeys[0];
				}
				}
			}

#if WITH_EDITOR
		AnimSequence->AddNewRawTrack(BoneName, &Pair.Value);
#else
		CompressionCodec->Tracks[BoneIndex] = Pair.Value;
#endif
		bHasTracks = true;
		}

	if (!bHasTracks)
	{
		AddError("LoadSkeletalAnimation()", "No Bone Tracks found in animation");
		return nullptr;
	}

#if WITH_EDITOR
	AnimSequence->PostProcessSequence();
#else
	AnimSequence->CompressedData.CompressedDataStructure = MakeUnique<FUECompressedAnimData>();
	AnimSequence->CompressedData.BoneCompressionCodec = CompressionCodec;
	AnimSequence->CompressedData.CurveCompressionCodec = NewObject<UAnimCurveCompressionCodec_CompressedRichCurve>();
	AnimSequence->PostLoad();
#endif

	return AnimSequence;
	}

bool FglTFRuntimeParser::LoadSkeletalAnimation_Internal(TSharedRef<FJsonObject> JsonAnimationObject, TMap<FString, FRawAnimSequenceTrack> & Tracks, float& Duration, TFunctionRef<bool(const FglTFRuntimeNode& Node)> Filter)
{

	auto Callback = [&](const FglTFRuntimeNode& Node, const FString& Path, const TArray<float> Timeline, const TArray<FVector4> Values)
	{
		int32 NumFrames = Duration * 30;

		float FrameDelta = 1.f / 30;

		if (Path == "rotation")
		{
			if (!Tracks.Contains(Node.Name))
			{
				Tracks.Add(Node.Name, FRawAnimSequenceTrack());
			}

			FRawAnimSequenceTrack& Track = Tracks[Node.Name];

			float FrameBase = 0.f;
			for (int32 Frame = 0; Frame < NumFrames; Frame++)
			{
				int32 FirstIndex;
				int32 SecondIndex;
				float Alpha = FindBestFrames(Timeline, FrameBase, FirstIndex, SecondIndex);
				FVector4 FirstQuatV = Values[FirstIndex];
				FVector4 SecondQuatV = Values[SecondIndex];
				FQuat FirstQuat = { FirstQuatV.X, FirstQuatV.Y, FirstQuatV.Z, FirstQuatV.W };
				FQuat SecondQuat = { SecondQuatV.X, SecondQuatV.Y, SecondQuatV.Z, SecondQuatV.W };
				FMatrix FirstMatrix = SceneBasis.Inverse() * FQuatRotationMatrix(FirstQuat) * SceneBasis;
				FMatrix SecondMatrix = SceneBasis.Inverse() * FQuatRotationMatrix(SecondQuat) * SceneBasis;
				FirstQuat = FirstMatrix.ToQuat();
				SecondQuat = SecondMatrix.ToQuat();
				FQuat AnimQuat = FQuat::Slerp(FirstQuat, SecondQuat, Alpha);
				Track.RotKeys.Add(AnimQuat);
				FrameBase += FrameDelta;
			}
		}
		else if (Path == "translation")
		{
			if (!Tracks.Contains(Node.Name))
			{
				Tracks.Add(Node.Name, FRawAnimSequenceTrack());
			}

			FRawAnimSequenceTrack& Track = Tracks[Node.Name];

			float FrameBase = 0.f;
			for (int32 Frame = 0; Frame < NumFrames; Frame++)
			{
				int32 FirstIndex;
				int32 SecondIndex;
				float Alpha = FindBestFrames(Timeline, FrameBase, FirstIndex, SecondIndex);
				FVector4 First = Values[FirstIndex];
				FVector4 Second = Values[SecondIndex];
				FVector AnimLocation = SceneBasis.TransformPosition(FMath::Lerp(First, Second, Alpha)) * SceneScale;
				Track.PosKeys.Add(AnimLocation);
				FrameBase += FrameDelta;
			}
		}
		else if (Path == "scale")
		{
			if (!Tracks.Contains(Node.Name))
			{
				Tracks.Add(Node.Name, FRawAnimSequenceTrack());
			}

			FRawAnimSequenceTrack& Track = Tracks[Node.Name];

			float FrameBase = 0.f;
			for (int32 Frame = 0; Frame < NumFrames; Frame++)
			{
				int32 FirstIndex;
				int32 SecondIndex;
				float Alpha = FindBestFrames(Timeline, FrameBase, FirstIndex, SecondIndex);
				FVector4 First = Values[FirstIndex];
				FVector4 Second = Values[SecondIndex];
				Track.ScaleKeys.Add((SceneBasis.Inverse() * FScaleMatrix(FMath::Lerp(First, Second, Alpha)) * SceneBasis).ExtractScaling());
				FrameBase += FrameDelta;
			}
		}
	};

	FString IgnoredName;
	return LoadAnimation_Internal(JsonAnimationObject, Duration, IgnoredName, Callback, Filter);
}

