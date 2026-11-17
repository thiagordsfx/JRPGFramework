#include "VFX/JRPGVFXSubsystem.h"
#include "VFX/JRPGVFXRecipe.h"
#include "Camera/CameraSubsystem.h"
#include "Core/CoreSubsystem.h"
#include "Components/MeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogJRPGVFX, Log, All);

namespace
{
	// Faixas reservadas (FJRPGVFXScalarTrack / FJRPGVFXColorTrack): não vão a material nem a user parameter
	const FName TrackScale(TEXT("Scale"));
	const FName TrackIntensity(TEXT("Intensity"));
	const FName TrackRadius(TEXT("Radius"));
	const FName TrackColor(TEXT("Color"));

	// User parameters que toda camada Niagara recebe
	const FName NiagaraColor(TEXT("Color"));
	const FName NiagaraEffectScale(TEXT("EffectScale"));
	const FName NiagaraTargetPosition(TEXT("TargetPosition"));
	const FName NiagaraInstigatorPosition(TEXT("InstigatorPosition"));
	const FName NiagaraBodyHeightScale(TEXT("BodyHeightScale"));

	bool LayerIsSpatial(EJRPGVFXLayerType Type)
	{
		return Type == EJRPGVFXLayerType::Niagara || Type == EJRPGVFXLayerType::Mesh || Type == EJRPGVFXLayerType::Light;
	}

	/** Uma camada sem duração própria numa receita em loop: não termina sozinha e as faixas repetem. */
	bool LayerLoops(const UJRPGVFXRecipe& Recipe, const FJRPGVFXLayer& Layer)
	{
		return Recipe.bLooping && Layer.Duration <= 0.0f;
	}

	// --- Console ---

	void PlayCommand(const TArray<FString>& Args, UWorld* World)
	{
		UJRPGVFXSubsystem* VFX = UJRPGVFXSubsystem::Get(World);
		if (!VFX || Args.Num() < 1)
		{
			UE_LOG(LogJRPGVFX, Warning, TEXT("JRPG.VFX.Play <Id do DT_VFX | caminho da receita> [escala]"));
			return;
		}
		const UJRPGVFXRecipe* Recipe = Args[0].StartsWith(TEXT("/"))
			? LoadObject<UJRPGVFXRecipe>(nullptr, *Args[0])
			: UJRPGVFXSubsystem::FindRecipe(FName(*Args[0]));
		if (!Recipe)
		{
			UE_LOG(LogJRPGVFX, Warning, TEXT("JRPG.VFX.Play: receita '%s' não encontrada"), *Args[0]);
			return;
		}
		const APlayerController* Controller = World->GetFirstPlayerController();
		APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;

		FJRPGVFXContext Context;
		Context.Instigator = Pawn;
		Context.Target = Pawn;
		Context.Location = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
		Context.Scale = Args.Num() > 1 ? FMath::Max(FCString::Atof(*Args[1]), 0.01f) : 1.0f;
		const FJRPGVFXHandle Handle = VFX->PlayEffect(Recipe, Context);
		UE_LOG(LogJRPGVFX, Log, TEXT("JRPG.VFX.Play: '%s' tocando (recibo %d, %.2fs%s)"), *Recipe->GetName(), Handle.Id,
			Recipe->GetLength(), Recipe->bLooping ? TEXT(", em loop: JRPG.VFX.StopAll para parar") : TEXT(""));
	}

	void StopAllCommand(const TArray<FString>& Args, UWorld* World)
	{
		if (UJRPGVFXSubsystem* VFX = UJRPGVFXSubsystem::Get(World))
		{
			// "soft" deixa cada efeito sumir no FadeOutTime dele
			VFX->StopAllEffects(!(Args.Num() > 0 && Args[0].Equals(TEXT("soft"), ESearchCase::IgnoreCase)));
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GVFXPlayCommand(
		TEXT("JRPG.VFX.Play"),
		TEXT("Toca um efeito no pawn do jogador: JRPG.VFX.Play <Id do DT_VFX | caminho da receita> [escala]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&PlayCommand));

	FAutoConsoleCommandWithWorldAndArgs GVFXStopAllCommand(
		TEXT("JRPG.VFX.StopAll"),
		TEXT("Para todos os efeitos: JRPG.VFX.StopAll [soft] (soft = cada um some no FadeOutTime da receita)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&StopAllCommand));
}

UJRPGVFXSubsystem* UJRPGVFXSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<UJRPGVFXSubsystem>() : nullptr;
}

bool UJRPGVFXSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// O mundo do editor de níveis fica de fora; os de preview entram para os notifies tocarem no editor de animação
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE
		|| WorldType == EWorldType::EditorPreview || WorldType == EWorldType::GamePreview;
}

void UJRPGVFXSubsystem::Deinitialize()
{
	// O mundo está saindo e os componentes vão com ele: nada a devolver (o pool do Niagara pode nem existir mais)
	Instances.Reset();
	FreeMeshes.Reset();
	FreeLights.Reset();
	FreePostProcesses.Reset();
	CustomDepthUsers.Reset();
	Host = nullptr;
	Super::Deinitialize();
}

TStatId UJRPGVFXSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UJRPGVFXSubsystem, STATGROUP_Tickables);
}

// --- API ---

FJRPGVFXHandle UJRPGVFXSubsystem::PlayEffect(const UJRPGVFXRecipe* Recipe, const FJRPGVFXContext& Context)
{
	FJRPGVFXHandle Handle;
	if (!Recipe || Recipe->Layers.IsEmpty() || !GetWorld())
	{
		return Handle;
	}

	FJRPGVFXInstance& Instance = Instances.AddDefaulted_GetRef();
	Instance.Id = NextId++;
	if (NextId <= 0)
	{
		NextId = 1;
	}
	Instance.Recipe = Recipe;
	Instance.Context = Context;
	Instance.Layers.SetNum(Recipe->Layers.Num());
	Handle.Id = Instance.Id;

	// As camadas do instante 0 entram já, no frame do disparo (um golpe não espera o próximo tick)
	for (int32 Index = 0; Index < Recipe->Layers.Num(); ++Index)
	{
		if (Recipe->Layers[Index].StartTime <= 0.0f)
		{
			StartLayer(Instance, Index);
		}
	}
	return Handle;
}

namespace
{
	const FJRPGVFXRow* FindVFXRow(FName EffectId)
	{
		if (EffectId.IsNone())
		{
			return nullptr;
		}
		const UDataTable* Table = UCoreSubsystem::ResolveDataTable(TEXT("DT_VFX"));
		return Table ? Table->FindRow<FJRPGVFXRow>(EffectId, TEXT("JRPGVFX"), false) : nullptr;
	}
}

FJRPGVFXHandle UJRPGVFXSubsystem::PlayEffectById(FName EffectId, const FJRPGVFXContext& Context)
{
	const FJRPGVFXRow* Row = FindVFXRow(EffectId);
	const UJRPGVFXRecipe* Recipe = Row ? Row->Recipe.LoadSynchronous() : nullptr;
	if (!Recipe)
	{
		UE_LOG(LogJRPGVFX, Warning, TEXT("PlayEffectById: '%s' não está no DT_VFX (ou a linha não tem receita)"), *EffectId.ToString());
		return FJRPGVFXHandle();
	}
	// Quem toca não deu cor: vale a da linha (a mesma receita em cada herói)
	FJRPGVFXContext Colored = Context;
	if (Colored.Color.Equals(FLinearColor::White))
	{
		Colored.Color = Row->Color;
	}
	return PlayEffect(Recipe, Colored);
}

UJRPGVFXRecipe* UJRPGVFXSubsystem::FindRecipe(FName EffectId)
{
	const FJRPGVFXRow* Row = FindVFXRow(EffectId);
	return Row ? Row->Recipe.LoadSynchronous() : nullptr;
}

void UJRPGVFXSubsystem::StopEffect(FJRPGVFXHandle Handle, bool bImmediate)
{
	if (!Handle.IsValid())
	{
		return;
	}
	for (int32 Index = 0; Index < Instances.Num(); ++Index)
	{
		FJRPGVFXInstance& Instance = Instances[Index];
		if (Instance.Id != Handle.Id)
		{
			continue;
		}
		const float FadeOut = Instance.Recipe ? Instance.Recipe->FadeOutTime : 0.0f;
		if (bImmediate || FadeOut <= 0.0f)
		{
			EndInstance(Instance, bImmediate);
			Instances.RemoveAtSwap(Index);
		}
		else if (!Instance.bStopping)
		{
			// Cedo demais para a receita: o fade espera o MinPlayTime (o Tick começa quando chegar)
			if (Instance.Elapsed < Instance.Recipe->MinPlayTime)
			{
				Instance.bStopQueued = true;
			}
			else
			{
				BeginSoftStop(Instance);
			}
		}
		return;
	}
}

void UJRPGVFXSubsystem::BeginSoftStop(FJRPGVFXInstance& Instance)
{
	Instance.bStopQueued = false;
	Instance.bStopping = true;
	Instance.StopElapsed = 0.0f;
	// O Niagara para de emitir já e as partículas vivas terminam sozinhas; o resto some no fade
	for (int32 Layer = 0; Layer < Instance.Layers.Num(); ++Layer)
	{
		if (Instance.Recipe->Layers[Layer].Type == EJRPGVFXLayerType::Niagara)
		{
			EndLayer(Instance, Layer, false);
		}
	}
}

void UJRPGVFXSubsystem::StopAllEffects(bool bImmediate)
{
	if (bImmediate)
	{
		for (FJRPGVFXInstance& Instance : Instances)
		{
			EndInstance(Instance, true);
		}
		Instances.Reset();
		return;
	}
	// Suave: de trás para frente, porque um efeito sem FadeOutTime sai da lista na hora
	for (int32 Index = Instances.Num() - 1; Index >= 0; --Index)
	{
		FJRPGVFXHandle Handle;
		Handle.Id = Instances[Index].Id;
		StopEffect(Handle, false);
	}
}

bool UJRPGVFXSubsystem::IsEffectPlaying(FJRPGVFXHandle Handle) const
{
	return Handle.IsValid() && Instances.ContainsByPredicate([&](const FJRPGVFXInstance& Instance) { return Instance.Id == Handle.Id; });
}

void UJRPGVFXSubsystem::SetEffectFloat(FJRPGVFXHandle Handle, FName Name, float Value, float Delay, float BlendTime)
{
	FJRPGVFXInstance* Instance = Handle.IsValid() && !Name.IsNone()
		? Instances.FindByPredicate([&](const FJRPGVFXInstance& Each) { return Each.Id == Handle.Id; })
		: nullptr;
	if (!Instance)
	{
		return;
	}
	FJRPGVFXFloatParam* Existing = Instance->FloatParams.Find(Name);
	FJRPGVFXFloatParam Param;
	// Parte de onde o valor está agora (no meio de um blend anterior, inclusive); um nome novo não tem de onde partir
	Param.From = Existing ? Existing->Evaluate() : Value;
	Param.To = Value;
	Param.Delay = FMath::Max(Delay, 0.0f);
	Param.BlendTime = FMath::Max(BlendTime, 0.0f);
	Instance->FloatParams.Add(Name, Param);
}

void UJRPGVFXSubsystem::PulseEffectFloat(FJRPGVFXHandle Handle, FName Name, float Peak, float RiseTime, float FallTime, float Rest, float Delay)
{
	FJRPGVFXInstance* Instance = Handle.IsValid() && !Name.IsNone()
		? Instances.FindByPredicate([&](const FJRPGVFXInstance& Each) { return Each.Id == Handle.Id; })
		: nullptr;
	if (!Instance)
	{
		return;
	}
	const FJRPGVFXFloatParam* Existing = Instance->FloatParams.Find(Name);
	FJRPGVFXFloatParam Param;
	Param.From = Existing ? Existing->Evaluate() : Rest;
	Param.To = Peak;
	Param.Delay = FMath::Max(Delay, 0.0f);
	Param.BlendTime = FMath::Max(RiseTime, 0.0f);
	Param.bPulse = true;
	Param.FallTime = FMath::Max(FallTime, 0.0f);
	Param.Rest = Rest;
	Instance->FloatParams.Add(Name, Param);
}

float UJRPGVFXSubsystem::GetEffectFloat(FJRPGVFXHandle Handle, FName Name, float Fallback) const
{
	const FJRPGVFXInstance* Instance = Handle.IsValid()
		? Instances.FindByPredicate([&](const FJRPGVFXInstance& Each) { return Each.Id == Handle.Id; })
		: nullptr;
	const FJRPGVFXFloatParam* Param = Instance ? Instance->FloatParams.Find(Name) : nullptr;
	return Param ? Param->Evaluate() : Fallback;
}

// --- Linha de tempo ---

float UJRPGVFXSubsystem::LayerAlpha(const UJRPGVFXRecipe& Recipe, const FJRPGVFXLayer& Layer, float Elapsed)
{
	const float Span = Layer.Duration > 0.0f ? Layer.Duration : Recipe.GetLength() - (Recipe.bLooping ? 0.0f : Layer.StartTime);
	if (Span <= UE_KINDA_SMALL_NUMBER)
	{
		return 1.0f;
	}
	const float Alpha = (Elapsed - Layer.StartTime) / Span;
	return LayerLoops(Recipe, Layer) ? FMath::Frac(FMath::Max(Alpha, 0.0f)) : FMath::Clamp(Alpha, 0.0f, 1.0f);
}

void UJRPGVFXSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	for (int32 Index = Instances.Num() - 1; Index >= 0; --Index)
	{
		FJRPGVFXInstance& Instance = Instances[Index];
		const UJRPGVFXRecipe* Recipe = Instance.Recipe;
		if (!Recipe || Instance.Layers.Num() != Recipe->Layers.Num())
		{
			// A receita foi editada com o efeito tocando (editor): encerra em vez de ler camadas que mudaram
			EndInstance(Instance, true);
			Instances.RemoveAtSwap(Index);
			continue;
		}

		// A linha de tempo da receita anda no TimeScale do disparo; o fade e o SetEffectFloat, no relógio real
		Instance.Elapsed += DeltaTime * FMath::Max(Instance.Context.TimeScale, 0.01f);
		for (TPair<FName, FJRPGVFXFloatParam>& Param : Instance.FloatParams)
		{
			Param.Value.Elapsed += DeltaTime;
		}
		if (Instance.bStopQueued && Instance.Elapsed >= Recipe->MinPlayTime)
		{
			BeginSoftStop(Instance);
		}
		float Fade = 1.0f;
		if (Instance.bStopping)
		{
			Instance.StopElapsed += DeltaTime;
			Fade = 1.0f - Instance.StopElapsed / FMath::Max(Recipe->FadeOutTime, UE_KINDA_SMALL_NUMBER);
			if (Fade <= 0.0f)
			{
				EndInstance(Instance, false);
				Instances.RemoveAtSwap(Index);
				continue;
			}
		}

		const float Length = Recipe->GetLength();
		for (int32 Layer = 0; Layer < Recipe->Layers.Num(); ++Layer)
		{
			const FJRPGVFXLayer& Data = Recipe->Layers[Layer];
			// StartLayer não mexe em Instance.Layers (o array não realoca aqui)
			if (!Instance.Layers[Layer].bStarted && !Instance.Layers[Layer].bDone && !Instance.bStopping && Instance.Elapsed >= Data.StartTime)
			{
				StartLayer(Instance, Layer);
			}
			const FJRPGVFXLayerState& State = Instance.Layers[Layer];
			if (!State.bStarted || State.bDone)
			{
				continue;
			}
			const bool bEnds = !LayerLoops(*Recipe, Data);
			const float End = Data.Duration > 0.0f ? Data.StartTime + Data.Duration : Length;
			if (bEnds && Instance.Elapsed >= End)
			{
				EndLayer(Instance, Layer, false);
			}
			else
			{
				UpdateLayer(Instance, Layer, LayerAlpha(*Recipe, Data, Instance.Elapsed), Fade);
			}
		}

		if (!Recipe->bLooping && !Instance.bStopping && Instance.Elapsed >= Length)
		{
			EndInstance(Instance, false);
			Instances.RemoveAtSwap(Index);
		}
	}
}

void UJRPGVFXSubsystem::EndInstance(FJRPGVFXInstance& Instance, bool bImmediate)
{
	if (!Instance.Recipe || Instance.Layers.Num() != Instance.Recipe->Layers.Num())
	{
		// Sem a receita das camadas só dá para devolver os componentes
		for (FJRPGVFXLayerState& State : Instance.Layers)
		{
			ReleaseComponent(State, true);
			State.bDone = true;
		}
		return;
	}
	for (int32 Layer = 0; Layer < Instance.Layers.Num(); ++Layer)
	{
		EndLayer(Instance, Layer, bImmediate);
	}
}

// --- Camadas ---

AActor* UJRPGVFXSubsystem::AnchorActor(const FJRPGVFXLayer& Layer, const FJRPGVFXContext& Context)
{
	switch (Layer.Anchor)
	{
	case EJRPGVFXAnchor::Target:     return IsValid(Context.Target) ? Context.Target.Get() : nullptr;
	case EJRPGVFXAnchor::Instigator: return IsValid(Context.Instigator) ? Context.Instigator.Get() : nullptr;
	default:                         return nullptr;
	}
}

FVector UJRPGVFXSubsystem::ActorCenter(const AActor* Actor, const FVector& Fallback)
{
	if (!IsValid(Actor))
	{
		return Fallback;
	}
	FVector Origin, Extent;
	Actor->GetActorBounds(false, Origin, Extent);
	// Um ator sem primitivas registradas devolve volume zero na origem do mundo
	return Extent.IsNearlyZero() ? Actor->GetActorLocation() : Origin;
}

bool UJRPGVFXSubsystem::BodyCenter(const AActor* Actor, FVector& OutCenter)
{
	if (!IsValid(Actor))
	{
		return false;
	}
	// A malha que manda na pose: não segue outra (as peças de arma seguem o corpo por Leader Pose) e tem mais ossos
	const USkeletalMeshComponent* Body = nullptr;
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(Actor);
	for (const USkeletalMeshComponent* Each : Meshes)
	{
		if (Each && Each->IsRegistered() && !Each->LeaderPoseComponent.IsValid()
			&& (!Body || Each->GetNumBones() > Body->GetNumBones()))
		{
			Body = Each;
		}
	}
	if (!Body)
	{
		return false;
	}
	const int32 NumBones = Body->GetNumBones();
	const TArray<FTransform>& Pose = Body->GetComponentSpaceTransforms();
	if (NumBones < 2 || Pose.Num() < NumBones)
	{
		return false;
	}
	// Sem o root (o osso 0 fica nos pés / na origem): as peças do corpo
	FVector Sum = FVector::ZeroVector;
	for (int32 Bone = 1; Bone < NumBones; ++Bone)
	{
		Sum += Pose[Bone].GetLocation();
	}
	OutCenter = Body->GetComponentTransform().TransformPosition(Sum / static_cast<double>(NumBones - 1));
	return true;
}

void UJRPGVFXSubsystem::FollowBody(const FJRPGVFXLayerState& State, UNiagaraComponent* Niagara)
{
	USceneComponent* Component = State.Component.Get();
	FVector Center;
	if (!Component || !BodyCenter(State.FollowActor.Get(), Center))
	{
		return;
	}
	const FVector Here = Component->GetComponentLocation();
	Component->SetWorldLocation(FVector(Center.X + State.FollowOffset.X, Center.Y + State.FollowOffset.Y, Here.Z));
	if (Niagara && State.FollowHeight > 1.0f)
	{
		Niagara->SetVariableFloat(NiagaraBodyHeightScale, FMath::Clamp((Center.Z - Here.Z) / State.FollowHeight, 0.2f, 2.0f));
	}
}

UJRPGVFXSubsystem::FAnchorPoint UJRPGVFXSubsystem::ResolveAnchor(const FJRPGVFXLayer& Layer, const FJRPGVFXContext& Context)
{
	FAnchorPoint Point;
	Point.Location = Context.Location;

	AActor* Actor = AnchorActor(Layer, Context);
	if (!Actor)
	{
		return Point;
	}
	Point.Actor = Actor;
	Point.Parent = Actor->GetRootComponent();
	Point.Location = Actor->GetActorLocation();
	Point.Rotation = FRotator(0.0, Actor->GetActorRotation().Yaw, 0.0);

	FVector Origin, Extent;
	Actor->GetActorBounds(false, Origin, Extent);
	const bool bHasBounds = !Extent.IsNearlyZero();
	if (Layer.bScaleByAnchorSize && bHasBounds)
	{
		Point.SizeScale = FMath::Max(Extent.X, Extent.Y) / FMath::Max(Layer.ReferenceSize, 1.0f);
	}

	if (!Layer.Socket.IsNone())
	{
		for (UActorComponent* Component : Actor->GetComponents())
		{
			USceneComponent* Scene = Cast<USceneComponent>(Component);
			if (Scene && Scene->DoesSocketExist(Layer.Socket))
			{
				Point.Parent = Scene;
				Point.Socket = Layer.Socket;
				Point.Location = Scene->GetSocketLocation(Layer.Socket);
				Point.Rotation = Scene->GetSocketRotation(Layer.Socket);
				return Point;
			}
		}
		UE_LOG(LogJRPGVFX, Verbose, TEXT("Camada '%s': socket '%s' não existe em %s; usando o Placement"),
			*Layer.Label.ToString(), *Layer.Socket.ToString(), *Actor->GetName());
	}

	if (bHasBounds)
	{
		if (Layer.Placement == EJRPGVFXPlacement::BoundsCenter)
		{
			Point.Location = Origin;
		}
		else if (Layer.Placement == EJRPGVFXPlacement::BoundsBottom)
		{
			Point.Location = FVector(Origin.X, Origin.Y, Origin.Z - Extent.Z);
		}
	}
	return Point;
}

void UJRPGVFXSubsystem::PlaceComponent(USceneComponent& Component, const FJRPGVFXLayer& Layer, const FAnchorPoint& Point, const FTransform& World)
{
	if (Layer.bAttach && Point.Parent)
	{
		Component.AttachToComponent(Point.Parent, FAttachmentTransformRules::KeepWorldTransform, Point.Socket);
	}
	Component.SetWorldTransform(World);
}

void UJRPGVFXSubsystem::StartLayer(FJRPGVFXInstance& Instance, int32 LayerIndex)
{
	const UJRPGVFXRecipe& Recipe = *Instance.Recipe;
	const FJRPGVFXLayer& Layer = Recipe.Layers[LayerIndex];
	FJRPGVFXLayerState& State = Instance.Layers[LayerIndex];
	State.bStarted = true;

	FAnchorPoint Point;
	FTransform World = FTransform::Identity;
	if (LayerIsSpatial(Layer.Type))
	{
		Point = ResolveAnchor(Layer, Instance.Context);
		const float Scale = FMath::Max(Instance.Context.Scale, 0.0f) * Point.SizeScale;
		// O deslocamento cresce junto com o efeito (um funil 2x maior fica 2x mais longe do chão)
		FTransform Local = Layer.Offset;
		Local.SetLocation(Local.GetLocation() * Scale);
		Local.SetScale3D(Local.GetScale3D() * Scale);
		if (Layer.bRandomYaw)
		{
			Local.ConcatenateRotation(FQuat(FVector::UpVector, CosmeticRng.FRandRange(0.0f, UE_TWO_PI)));
		}
		World = Local * FTransform(Point.Rotation, Point.Location);
		State.BaseScale = World.GetScale3D();

		// Seguir o corpo: nasce já sobre ele (só o Offset da camada afasta) e guarda a altura do centro para o BodyHeightScale
		FVector Center;
		if (Layer.bFollowBody && BodyCenter(Point.Actor, Center))
		{
			State.FollowActor = Point.Actor;
			State.FollowOffset = FVector2D(World.GetLocation() - Point.Location);
			State.FollowHeight = Center.Z - World.GetLocation().Z;
			World.SetLocation(FVector(Center.X + State.FollowOffset.X, Center.Y + State.FollowOffset.Y, World.GetLocation().Z));
		}
	}

	switch (Layer.Type)
	{
	case EJRPGVFXLayerType::Niagara:
	{
		if (!Layer.System)
		{
			State.bDone = true;
			return;
		}
		// Nasce desligado: os user parameters entram antes da primeira simulação
		UNiagaraComponent* Niagara = (Layer.bAttach && Point.Parent)
			? UNiagaraFunctionLibrary::SpawnSystemAttached(Layer.System, Point.Parent, Point.Socket, World.GetLocation(),
				World.Rotator(), World.GetScale3D(), EAttachLocation::KeepWorldPosition, false, ENCPoolMethod::ManualRelease, false, false)
			: UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, Layer.System, World.GetLocation(), World.Rotator(),
				World.GetScale3D(), false, false, ENCPoolMethod::ManualRelease, false);
		if (!Niagara)
		{
			State.bDone = true;
			return;
		}
		Niagara->SetWorldScale3D(World.GetScale3D());
		// A camada que termina acompanha a linha de tempo esticada; a que fica em loop simula a 1. Sempre escrito: o componente
		// vem do pool com a dilatação do disparo anterior
		Niagara->SetCustomTimeDilation(LayerLoops(Recipe, Layer) ? 1.0f : FMath::Max(Instance.Context.TimeScale, 0.01f));
		// Branco = sem tinta: o sistema fica com a cor dele (o User.Color HDR de cada herói no Spirit)
		if (!Instance.Context.Color.Equals(FLinearColor::White))
		{
			Niagara->SetVariableLinearColor(NiagaraColor, Instance.Context.Color);
		}
		Niagara->SetVariableFloat(NiagaraEffectScale, FMath::Max(Instance.Context.Scale, 0.0f) * Point.SizeScale);
		if (!Layer.SkeletalMeshParameter.IsNone() && Point.Actor)
		{
			if (USkeletalMeshComponent* Body = Point.Actor->FindComponentByClass<USkeletalMeshComponent>())
			{
				UNiagaraFunctionLibrary::OverrideSystemUserVariableSkeletalMeshComponent(Niagara, Layer.SkeletalMeshParameter.ToString(), Body);
			}
		}
		State.Component = Niagara;
		UpdateLayer(Instance, LayerIndex, LayerAlpha(Recipe, Layer, Instance.Elapsed), 1.0f);
		Niagara->Activate(true);
		return;
	}
	case EJRPGVFXLayerType::Mesh:
	{
		UStaticMeshComponent* Mesh = Layer.Mesh ? AcquireMesh() : nullptr;
		if (!Mesh)
		{
			State.bDone = true;
			return;
		}
		Mesh->EmptyOverrideMaterials();
		Mesh->SetStaticMesh(Layer.Mesh);
		if (Layer.Material)
		{
			for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
			{
				Mesh->SetMaterial(Slot, Layer.Material);
			}
		}
		PlaceComponent(*Mesh, Layer, Point, World);
		Mesh->SetScalarParameterValueOnMaterials(Recipe.FadeParameter, 1.0f);
		State.Component = Mesh;
		UpdateLayer(Instance, LayerIndex, LayerAlpha(Recipe, Layer, Instance.Elapsed), 1.0f);
		Mesh->SetVisibility(true);
		return;
	}
	case EJRPGVFXLayerType::Light:
	{
		UPointLightComponent* Light = AcquireLight();
		if (!Light)
		{
			State.bDone = true;
			return;
		}
		Light->SetLightColor(Layer.LightColor);
		PlaceComponent(*Light, Layer, Point, World);
		State.Component = Light;
		UpdateLayer(Instance, LayerIndex, LayerAlpha(Recipe, Layer, Instance.Elapsed), 1.0f);
		Light->SetVisibility(true);
		return;
	}
	case EJRPGVFXLayerType::BodyParam:
	{
		State.BodyActor = AnchorActor(Layer, Instance.Context);
		if (!State.BodyActor.IsValid())
		{
			State.bDone = true;
			return;
		}
		UpdateLayer(Instance, LayerIndex, LayerAlpha(Recipe, Layer, Instance.Elapsed), 1.0f);
		return;
	}
	case EJRPGVFXLayerType::PostProcess:
	{
		UPostProcessComponent* Post = Layer.Material ? AcquirePostProcess() : nullptr;
		UMaterialInstanceDynamic* Material = Post ? UMaterialInstanceDynamic::Create(Layer.Material, Post) : nullptr;
		if (!Material)
		{
			if (Post)
			{
				ReleasePostProcess(Post);
			}
			State.bDone = true;
			return;
		}
		Material->SetScalarParameterValue(Recipe.FadeParameter, 1.0f);
		// O material lê a silhueta do ator pela Custom Depth: sem ator âncora ele roda, mas não acha corpo nenhum
		AcquireCustomDepth(AnchorActor(Layer, Instance.Context), State);
		State.Component = Post;
		State.PostMaterial = Material;
		UpdateLayer(Instance, LayerIndex, LayerAlpha(Recipe, Layer, Instance.Elapsed), 1.0f);
		Post->Settings.WeightedBlendables.Array.Reset();
		Post->Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, Material));
		Post->bEnabled = true;
		return;
	}
	case EJRPGVFXLayerType::CameraShake:
	{
		// Um disparo só. Sem GameInstance (preview de animação) não há câmera de jogo para tremer
		const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
		if (UCameraSubsystem* Camera = GameInstance ? GameInstance->GetSubsystem<UCameraSubsystem>() : nullptr)
		{
			Camera->PlayCameraShake(Layer.ShakePreset, Layer.ShakeScale);
		}
		State.bDone = true;
		return;
	}
	default:
		State.bDone = true;
		return;
	}
}

void UJRPGVFXSubsystem::UpdateLayer(FJRPGVFXInstance& Instance, int32 LayerIndex, float Alpha, float Fade)
{
	const UJRPGVFXRecipe& Recipe = *Instance.Recipe;
	const FJRPGVFXLayer& Layer = Recipe.Layers[LayerIndex];
	const FJRPGVFXLayerState& State = Instance.Layers[LayerIndex];
	const float ClampedFade = FMath::Clamp(Fade, 0.0f, 1.0f);

	USceneComponent* Component = State.Component.Get();
	UNiagaraComponent* Niagara = Layer.Type == EJRPGVFXLayerType::Niagara ? Cast<UNiagaraComponent>(Component) : nullptr;
	UMeshComponent* Mesh = Layer.Type == EJRPGVFXLayerType::Mesh ? Cast<UMeshComponent>(Component) : nullptr;
	UPointLightComponent* Light = Layer.Type == EJRPGVFXLayerType::Light ? Cast<UPointLightComponent>(Component) : nullptr;
	AActor* Body = Layer.Type == EJRPGVFXLayerType::BodyParam ? State.BodyActor.Get() : nullptr;
	UMaterialInstanceDynamic* Post = Layer.Type == EJRPGVFXLayerType::PostProcess ? State.PostMaterial.Get() : nullptr;
	if (!Niagara && !Mesh && !Light && !Body && !Post)
	{
		return;
	}
	if (State.FollowActor.IsValid())
	{
		FollowBody(State, Niagara);
	}
	// As faixas com chaves leem o relógio da camada, sem repetir numa receita em loop
	const float Seconds = FMath::Max(Instance.Elapsed - Layer.StartTime, 0.0f);

	float ScaleMul = 1.0f;
	float IntensityMul = 1.0f;
	float RadiusMul = 1.0f;
	bool bHasScale = false;

	for (const FJRPGVFXScalarTrack& Track : Layer.ScalarTracks)
	{
		if (Track.Name.IsNone())
		{
			continue;
		}
		const float Value = Track.EvaluateAt(Alpha, Seconds);
		if (Track.Name == TrackScale)
		{
			ScaleMul = Value;
			bHasScale = true;
		}
		else if (Light)
		{
			if (Track.Name == TrackIntensity) { IntensityMul = Value; }
			else if (Track.Name == TrackRadius) { RadiusMul = Value; }
		}
		else if (Niagara)
		{
			Niagara->SetVariableFloat(Track.Name, Value);
		}
		else if (Mesh)
		{
			Mesh->SetScalarParameterValueOnMaterials(Track.Name, Value);
		}
		else if (Post)
		{
			Post->SetScalarParameterValue(Track.Name, Value);
		}
		else if (Body)
		{
			// No StopEffect suave o corpo volta ao repouso (From) junto com o resto
			SetBodyScalar(Body, Track.Name, FMath::Lerp(Track.From, Value, ClampedFade));
		}
	}

	for (const FJRPGVFXColorTrack& Track : Layer.ColorTracks)
	{
		if (Track.Name.IsNone())
		{
			continue;
		}
		const FLinearColor Value = Track.Evaluate(Alpha, Instance.Context.Color);
		if (Light)
		{
			if (Track.Name == TrackColor) { Light->SetLightColor(Value); }
		}
		else if (Niagara)
		{
			Niagara->SetVariableLinearColor(Track.Name, Value);
		}
		else if (Mesh)
		{
			Mesh->SetColorParameterValueOnMaterials(Track.Name, Value);
		}
		else if (Post)
		{
			Post->SetVectorParameterValue(Track.Name, Value);
		}
		else if (Body)
		{
			const FLinearColor Rest = Track.Evaluate(0.0f, Instance.Context.Color);
			SetBodyColor(Body, Track.Name, FMath::Lerp(Rest, Value, ClampedFade));
		}
	}

	// Os parâmetros de SetEffectFloat vencem as faixas de mesmo nome
	for (const TPair<FName, FJRPGVFXFloatParam>& Param : Instance.FloatParams)
	{
		if (Niagara)
		{
			Niagara->SetVariableFloat(Param.Key, Param.Value.Evaluate());
		}
		else if (Mesh)
		{
			Mesh->SetScalarParameterValueOnMaterials(Param.Key, Param.Value.Evaluate());
		}
		else if (Post)
		{
			Post->SetScalarParameterValue(Param.Key, Param.Value.Evaluate());
		}
	}

	if (Component && bHasScale)
	{
		Component->SetWorldScale3D(State.BaseScale * ScaleMul);
	}
	if (Light)
	{
		Light->SetIntensity(Layer.LightIntensity * IntensityMul * ClampedFade);
		Light->SetAttenuationRadius(Layer.LightRadius * RadiusMul * FMath::Max(State.BaseScale.GetMax(), UE_KINDA_SMALL_NUMBER) * (bHasScale ? ScaleMul : 1.0f));
	}
	if (Mesh && Fade < 1.0f)
	{
		Mesh->SetScalarParameterValueOnMaterials(Recipe.FadeParameter, ClampedFade);
	}
	if (Post && Fade < 1.0f)
	{
		Post->SetScalarParameterValue(Recipe.FadeParameter, ClampedFade);
	}
	if (Niagara)
	{
		const FVector Here = Niagara->GetComponentLocation();
		Niagara->SetVariablePosition(NiagaraTargetPosition, ActorCenter(Instance.Context.Target, Here));
		Niagara->SetVariablePosition(NiagaraInstigatorPosition, ActorCenter(Instance.Context.Instigator, Here));
	}
}

void UJRPGVFXSubsystem::EndLayer(FJRPGVFXInstance& Instance, int32 LayerIndex, bool bImmediate)
{
	FJRPGVFXLayerState& State = Instance.Layers[LayerIndex];
	if (State.bDone)
	{
		return;
	}
	if (State.bStarted)
	{
		const UJRPGVFXRecipe& Recipe = *Instance.Recipe;
		const FJRPGVFXLayer& Layer = Recipe.Layers[LayerIndex];
		if (Layer.Type == EJRPGVFXLayerType::BodyParam)
		{
			// O corpo fica com o valor final das faixas; numa camada em loop, com o de repouso (From)
			UpdateLayer(Instance, LayerIndex, LayerLoops(Recipe, Layer) ? 0.0f : 1.0f, LayerLoops(Recipe, Layer) ? 0.0f : 1.0f);
		}
		ReleaseComponent(State, bImmediate);
	}
	State.bDone = true;
}

void UJRPGVFXSubsystem::ReleaseComponent(FJRPGVFXLayerState& State, bool bImmediate)
{
	USceneComponent* Component = State.Component.Get();
	State.Component.Reset();
	State.PostMaterial.Reset();
	ReleaseCustomDepth(State);
	if (!Component)
	{
		return;
	}
	if (UNiagaraComponent* Niagara = Cast<UNiagaraComponent>(Component))
	{
		// Suave: para de emitir e volta ao pool do Niagara quando a última partícula morrer
		if (bImmediate)
		{
			Niagara->DeactivateImmediate();
		}
		else
		{
			Niagara->Deactivate();
		}
		Niagara->ReleaseToPool();
	}
	else if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
	{
		ReleaseMesh(Mesh);
	}
	else if (UPointLightComponent* Light = Cast<UPointLightComponent>(Component))
	{
		ReleaseLight(Light);
	}
	else if (UPostProcessComponent* Post = Cast<UPostProcessComponent>(Component))
	{
		ReleasePostProcess(Post);
	}
}

// --- Corpo ---

void UJRPGVFXSubsystem::SetBodyScalar(AActor* Actor, FName Name, float Value)
{
	if (!IsValid(Actor))
	{
		return;
	}
	// Todas as malhas do ator: o corpo e as peças que seguem a pose dele (como o JRPGDeathFade do pawn de luta)
	TInlineComponentArray<UMeshComponent*> Meshes(Actor);
	for (UMeshComponent* Mesh : Meshes)
	{
		Mesh->SetScalarParameterValueOnMaterials(Name, Value);
	}
}

void UJRPGVFXSubsystem::SetBodyColor(AActor* Actor, FName Name, const FLinearColor& Value)
{
	if (!IsValid(Actor))
	{
		return;
	}
	TInlineComponentArray<UMeshComponent*> Meshes(Actor);
	for (UMeshComponent* Mesh : Meshes)
	{
		Mesh->SetColorParameterValueOnMaterials(Name, Value);
	}
}

// --- Pool ---

AActor* UJRPGVFXSubsystem::EnsureHost()
{
	if (IsValid(Host))
	{
		return Host;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (Host)
	{
		USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"), RF_Transient);
		Host->SetRootComponent(Root);
		Root->RegisterComponent();
#if WITH_EDITOR
		Host->SetActorLabel(TEXT("JRPGVFXHost"));
#endif
	}
	// Os componentes do hospedeiro antigo (mundo trocado) foram com ele
	FreeMeshes.Reset();
	FreeLights.Reset();
	FreePostProcesses.Reset();
	return Host;
}

UStaticMeshComponent* UJRPGVFXSubsystem::AcquireMesh()
{
	while (FreeMeshes.Num() > 0)
	{
		if (UStaticMeshComponent* Pooled = FreeMeshes.Pop(EAllowShrinking::No))
		{
			if (IsValid(Pooled))
			{
				return Pooled;
			}
		}
	}
	AActor* Owner = EnsureHost();
	if (!Owner)
	{
		return nullptr;
	}
	UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(Owner, NAME_None, RF_Transient);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Mesh->bReceivesDecals = false;
	// Fora do distance field: a névoa (JRPGMist) e a iluminação indireta não devem contornar um efeito
	Mesh->bAffectDistanceFieldLighting = false;
	Mesh->bAffectDynamicIndirectLighting = false;
	Mesh->SetVisibility(false);
	Mesh->SetupAttachment(Owner->GetRootComponent());
	Mesh->RegisterComponent();
	return Mesh;
}

UPointLightComponent* UJRPGVFXSubsystem::AcquireLight()
{
	while (FreeLights.Num() > 0)
	{
		if (UPointLightComponent* Pooled = FreeLights.Pop(EAllowShrinking::No))
		{
			if (IsValid(Pooled))
			{
				return Pooled;
			}
		}
	}
	AActor* Owner = EnsureHost();
	if (!Owner)
	{
		return nullptr;
	}
	UPointLightComponent* Light = NewObject<UPointLightComponent>(Owner, NAME_None, RF_Transient);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetIntensityUnits(ELightUnits::Unitless);
	Light->SetCastShadows(false);
	Light->SetIntensity(0.0f);
	Light->SetVisibility(false);
	Light->SetupAttachment(Owner->GetRootComponent());
	Light->RegisterComponent();
	return Light;
}

void UJRPGVFXSubsystem::ReleaseMesh(UStaticMeshComponent* Component)
{
	if (!IsValid(Component) || !IsValid(Host))
	{
		return;
	}
	Component->SetVisibility(false);
	Component->AttachToComponent(Host->GetRootComponent(), FAttachmentTransformRules::SnapToTargetIncludingScale);
	// Solta a malha e os materiais (e os MIDs deles) até o próximo uso
	Component->EmptyOverrideMaterials();
	Component->SetStaticMesh(nullptr);
	FreeMeshes.Add(Component);
}

void UJRPGVFXSubsystem::ReleaseLight(UPointLightComponent* Component)
{
	if (!IsValid(Component) || !IsValid(Host))
	{
		return;
	}
	Component->SetVisibility(false);
	Component->SetIntensity(0.0f);
	Component->AttachToComponent(Host->GetRootComponent(), FAttachmentTransformRules::SnapToTargetIncludingScale);
	FreeLights.Add(Component);
}

UPostProcessComponent* UJRPGVFXSubsystem::AcquirePostProcess()
{
	while (FreePostProcesses.Num() > 0)
	{
		if (UPostProcessComponent* Pooled = FreePostProcesses.Pop(EAllowShrinking::No))
		{
			if (IsValid(Pooled))
			{
				return Pooled;
			}
		}
	}
	AActor* Owner = EnsureHost();
	if (!Owner)
	{
		return nullptr;
	}
	UPostProcessComponent* Post = NewObject<UPostProcessComponent>(Owner, NAME_None, RF_Transient);
	// Sem volume: vale para qualquer câmera do mundo, somado aos pós-processos que já existem (é um blendable)
	Post->bUnbound = true;
	Post->bEnabled = false;
	Post->BlendWeight = 1.0f;
	Post->SetupAttachment(Owner->GetRootComponent());
	Post->RegisterComponent();
	return Post;
}

void UJRPGVFXSubsystem::ReleasePostProcess(UPostProcessComponent* Component)
{
	if (!IsValid(Component) || !IsValid(Host))
	{
		return;
	}
	// Desligado a engine nem roda o material; o blendable (e o MID dele) é solto até o próximo uso
	Component->bEnabled = false;
	Component->Settings.WeightedBlendables.Array.Reset();
	FreePostProcesses.Add(Component);
}

void UJRPGVFXSubsystem::AcquireCustomDepth(AActor* Actor, FJRPGVFXLayerState& State)
{
	if (!IsValid(Actor))
	{
		return;
	}
	// Todas as malhas do ator: o corpo e as peças que seguem a pose dele
	TInlineComponentArray<UMeshComponent*> Meshes(Actor);
	for (UMeshComponent* Mesh : Meshes)
	{
		const TWeakObjectPtr<UPrimitiveComponent> Key(Mesh);
		int32* Users = CustomDepthUsers.Find(Key);
		if (!Users)
		{
			if (Mesh->bRenderCustomDepth)
			{
				continue;   // já estava ligado por outro motivo: não é nosso para desligar
			}
			Mesh->SetRenderCustomDepth(true);
			Users = &CustomDepthUsers.Add(Key, 0);
		}
		++(*Users);
		State.CustomDepthMeshes.Add(Key);
	}
}

void UJRPGVFXSubsystem::ReleaseCustomDepth(FJRPGVFXLayerState& State)
{
	for (const TWeakObjectPtr<UPrimitiveComponent>& Entry : State.CustomDepthMeshes)
	{
		int32* Users = CustomDepthUsers.Find(Entry);
		if (!Users)
		{
			continue;
		}
		if (--(*Users) <= 0)
		{
			if (UPrimitiveComponent* Mesh = Entry.Get())
			{
				Mesh->SetRenderCustomDepth(false);
			}
			CustomDepthUsers.Remove(Entry);
		}
	}
	State.CustomDepthMeshes.Reset();
}
