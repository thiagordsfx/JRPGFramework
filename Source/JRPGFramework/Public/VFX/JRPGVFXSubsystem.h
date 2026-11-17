#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VFX/JRPGVFXTypes.h"
#include "JRPGVFXSubsystem.generated.h"

class UJRPGVFXRecipe;
class UMaterialInstanceDynamic;
class UMeshComponent;
class UNiagaraComponent;
class UPointLightComponent;
class UPostProcessComponent;
class UPrimitiveComponent;
class USceneComponent;
class UStaticMeshComponent;

/** O estado de uma camada de um efeito tocando (fora da reflexão: os componentes são do ator hospedeiro ou do pool do Niagara). */
struct FJRPGVFXLayerState
{
	/** O componente da camada: UNiagaraComponent, UStaticMeshComponent ou UPointLightComponent. Nulo em BodyParam / CameraShake. */
	TWeakObjectPtr<USceneComponent> Component;
	/** O ator cujos materiais a camada BodyParam escreve. */
	TWeakObjectPtr<AActor> BodyActor;
	/** PostProcess: o material da camada (o blendable do componente de pós-processo em Component). */
	TWeakObjectPtr<UMaterialInstanceDynamic> PostMaterial;
	/** PostProcess: as malhas do ator âncora em que a camada pediu o Render CustomDepth Pass. */
	TArray<TWeakObjectPtr<UPrimitiveComponent>> CustomDepthMeshes;
	/** A escala no mundo com que a camada nasceu (a faixa "Scale" multiplica isto). */
	FVector BaseScale = FVector::OneVector;
	/** bFollowBody: o ator seguido, o deslocamento em XY da camada até o centro do corpo e a altura do centro sobre ela no começo. */
	TWeakObjectPtr<AActor> FollowActor;
	FVector2D FollowOffset = FVector2D::ZeroVector;
	float FollowHeight = 0.0f;
	bool bStarted = false;
	bool bDone = false;
};

/**
 * Um parâmetro float mudado com o efeito tocando (SetEffectFloat): espera Delay e vai de From a To em BlendTime. Num pulso
 * (PulseEffectFloat) depois volta de To a Rest em FallTime.
 */
struct FJRPGVFXFloatParam
{
	float From = 0.0f;
	float To = 0.0f;
	float Delay = 0.0f;
	float BlendTime = 0.0f;
	float Elapsed = 0.0f;
	bool bPulse = false;
	float FallTime = 0.0f;
	float Rest = 0.0f;

	float Evaluate() const
	{
		const float Time = Elapsed - Delay;
		if (bPulse && Time >= BlendTime)
		{
			const float Fall = FallTime > 0.0f ? (Time - BlendTime) / FallTime : 1.0f;
			return FMath::Lerp(To, Rest, FMath::SmoothStep(0.0f, 1.0f, Fall));
		}
		const float Alpha = BlendTime > 0.0f ? Time / BlendTime : (Time >= 0.0f ? 1.0f : 0.0f);
		return FMath::Lerp(From, To, FMath::SmoothStep(0.0f, 1.0f, Alpha));
	}
};

/** Um efeito tocando. */
USTRUCT()
struct FJRPGVFXInstance
{
	GENERATED_BODY()

	int32 Id = 0;

	/** Os parâmetros de SetEffectFloat: escritos em toda camada Niagara (user parameter) e Mesh (material) a cada frame. */
	TMap<FName, FJRPGVFXFloatParam> FloatParams;

	UPROPERTY()
	TObjectPtr<const UJRPGVFXRecipe> Recipe;

	UPROPERTY()
	FJRPGVFXContext Context;

	TArray<FJRPGVFXLayerState> Layers;
	float Elapsed = 0.0f;
	/** StopEffect suave pedido: o efeito some em FadeOutTime. */
	bool bStopping = false;
	float StopElapsed = 0.0f;
	/** StopEffect suave pedido antes do MinPlayTime da receita: o fade começa quando Elapsed chegar lá. */
	bool bStopQueued = false;
};

/**
 * UJRPGVFXSubsystem
 * Toca os efeitos visuais do jogo a partir de receitas (UJRPGVFXRecipe): cada efeito é uma linha de tempo de camadas —
 * malhas com shader (a forma), sistemas Niagara (as partículas), luzes, parâmetros no corpo dos atores e shakes de câmera.
 * O subsistema cria, move, anima e recolhe os componentes; a aparência vem dos assets que a receita aponta.
 *
 * Quem dispara: a apresentação da luta e os clips (UJRPGAnimNotify_VFX), nos pontos em que o original dispara os efeitos
 * dele. A lógica da luta não sabe que isto existe, e a aleatoriedade daqui vem de um stream cosmético, nunca do Rng da luta.
 *
 * Niagara: os user parameters abaixo são escritos em toda camada Niagara, se o sistema os tiver —
 *   Color (LinearColor) e EffectScale (float): do contexto, no disparo;
 *   TargetPosition e InstigatorPosition (Position): o centro do alvo e de quem causou, a cada frame (um enxame que voa até
 *   o herói segue o herói).
 *
 * As malhas e as luzes vêm de um pool num ator hospedeiro por mundo; o Niagara usa o pool dele.
 *
 * Console (PIE): JRPG.VFX.Play <Id do DT_VFX | caminho da receita> [escala] toca no pawn do jogador;
 * JRPG.VFX.StopAll para tudo.
 */
UCLASS()
class JRPGFRAMEWORK_API UJRPGVFXSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** O subsistema do mundo de um objeto (nulo sem mundo). */
	static UJRPGVFXSubsystem* Get(const UObject* WorldContextObject);

	/** Toca a receita. O recibo só importa para parar um efeito em loop; um efeito comum termina sozinho. */
	UFUNCTION(BlueprintCallable, Category = "JRPG|VFX")
	FJRPGVFXHandle PlayEffect(const UJRPGVFXRecipe* Recipe, const FJRPGVFXContext& Context);

	/**
	 * Toca a receita da linha EffectId do DT_VFX. Recibo inválido se a linha ou a receita não existem.
	 * Um Context.Color branco recebe a Color da linha.
	 */
	UFUNCTION(BlueprintCallable, Category = "JRPG|VFX")
	FJRPGVFXHandle PlayEffectById(FName EffectId, const FJRPGVFXContext& Context);

	/** A receita da linha EffectId do DT_VFX (carrega se preciso); nula se não há. */
	UFUNCTION(BlueprintCallable, Category = "JRPG|VFX")
	static UJRPGVFXRecipe* FindRecipe(FName EffectId);

	/** Para o efeito: suave (some no FadeOutTime da receita) ou na hora. */
	UFUNCTION(BlueprintCallable, Category = "JRPG|VFX")
	void StopEffect(FJRPGVFXHandle Handle, bool bImmediate = false);

	/** Para tudo (fim de luta, troca de cena). */
	UFUNCTION(BlueprintCallable, Category = "JRPG|VFX")
	void StopAllEffects(bool bImmediate = true);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "JRPG|VFX")
	bool IsEffectPlaying(FJRPGVFXHandle Handle) const;

	/**
	 * Muda um parâmetro float do efeito tocando: o user parameter Name de toda camada Niagara e o parâmetro Name dos
	 * materiais das camadas Mesh e PostProcess, inclusive as que ainda vão entrar. Espera Delay segundos e vai do valor atual
	 * ao novo em BlendTime. O primeiro valor de um nome não tem de onde partir: entra direto, depois do Delay. Vence uma faixa
	 * da receita com o mesmo nome. Relógio real (não o TimeScale).
	 */
	UFUNCTION(BlueprintCallable, Category = "JRPG|VFX")
	void SetEffectFloat(FJRPGVFXHandle Handle, FName Name, float Value, float Delay = 0.0f, float BlendTime = 0.0f);

	/**
	 * Um pulso num parâmetro float (a piscada, o estouro da chama): depois de Delay vai do valor atual ao Peak em RiseTime e
	 * volta ao Rest em FallTime, onde fica. Um nome que nunca foi escrito parte do Rest. Mesmos destinos do SetEffectFloat.
	 */
	UFUNCTION(BlueprintCallable, Category = "JRPG|VFX")
	void PulseEffectFloat(FJRPGVFXHandle Handle, FName Name, float Peak, float RiseTime, float FallTime, float Rest = 0.0f, float Delay = 0.0f);

	/** O valor atual de um parâmetro de SetEffectFloat (Fallback se o efeito não toca ou o nome nunca foi escrito). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "JRPG|VFX")
	float GetEffectFloat(FJRPGVFXHandle Handle, FName Name, float Fallback = 0.0f) const;

	/** Quantos efeitos estão tocando (testes e debug). */
	int32 GetNumActiveEffects() const { return Instances.Num(); }

	// UWorldSubsystem / FTickableGameObject
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	/** O preview de animação também toca os efeitos dos notifies. */
	virtual bool IsTickableInEditor() const override { return true; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	/** Onde uma camada nasce. */
	struct FAnchorPoint
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		/** A quem a camada se prende (nulo = ao mundo). */
		USceneComponent* Parent = nullptr;
		FName Socket;
		AActor* Actor = nullptr;
		/** O fator de bScaleByAnchorSize (1 sem ele). */
		float SizeScale = 1.0f;
	};

	static AActor* AnchorActor(const FJRPGVFXLayer& Layer, const FJRPGVFXContext& Context);
	static FAnchorPoint ResolveAnchor(const FJRPGVFXLayer& Layer, const FJRPGVFXContext& Context);
	/** O centro do volume do ator (Fallback sem ator). */
	static FVector ActorCenter(const AActor* Actor, const FVector& Fallback);
	/**
	 * O centro do corpo do ator: a média dos ossos (sem o root) da malha esquelética principal dele (a que não segue outra
	 * por Leader Pose e tem mais ossos). Falso se o ator não tem malha esquelética com ossos.
	 */
	static bool BodyCenter(const AActor* Actor, FVector& OutCenter);
	/** bFollowBody: leva a camada ao corpo (XY) e escreve o BodyHeightScale no Niagara. */
	static void FollowBody(const FJRPGVFXLayerState& State, UNiagaraComponent* Niagara);

	void StartLayer(FJRPGVFXInstance& Instance, int32 LayerIndex);
	/**
	 * Escreve as faixas da camada no tempo Alpha (0..1), com o Fade do StopEffect suave (1 = inteiro). As faixas com Keys
	 * leem os segundos desde o começo da camada (Instance.Elapsed - StartTime).
	 */
	void UpdateLayer(FJRPGVFXInstance& Instance, int32 LayerIndex, float Alpha, float Fade);
	/** Deixa o corpo no valor final das faixas e devolve o componente (bImmediate: as partículas vivas somem junto). */
	void EndLayer(FJRPGVFXInstance& Instance, int32 LayerIndex, bool bImmediate);
	void EndInstance(FJRPGVFXInstance& Instance, bool bImmediate);
	/** Começa o fade de um StopEffect suave: o Niagara para de emitir, o resto some em FadeOutTime. */
	void BeginSoftStop(FJRPGVFXInstance& Instance);
	void ReleaseComponent(FJRPGVFXLayerState& State, bool bImmediate);

	/** O tempo 0..1 da camada no instante Elapsed do efeito. */
	static float LayerAlpha(const UJRPGVFXRecipe& Recipe, const FJRPGVFXLayer& Layer, float Elapsed);

	AActor* EnsureHost();
	UStaticMeshComponent* AcquireMesh();
	UPointLightComponent* AcquireLight();
	UPostProcessComponent* AcquirePostProcess();
	void ReleaseMesh(UStaticMeshComponent* Component);
	void ReleaseLight(UPointLightComponent* Component);
	void ReleasePostProcess(UPostProcessComponent* Component);
	/**
	 * Liga o Render CustomDepth Pass nas malhas do ator para uma camada PostProcess, e desliga quando a última camada que
	 * pediu soltar. Uma malha que já tinha o passe ligado por outro motivo fica como estava.
	 */
	void AcquireCustomDepth(AActor* Actor, FJRPGVFXLayerState& State);
	void ReleaseCustomDepth(FJRPGVFXLayerState& State);
	/** Põe o componente do pool no ponto de apoio (e preso a ele, se a camada pede). */
	static void PlaceComponent(USceneComponent& Component, const FJRPGVFXLayer& Layer, const FAnchorPoint& Point, const FTransform& World);

	static void SetBodyScalar(AActor* Actor, FName Name, float Value);
	static void SetBodyColor(AActor* Actor, FName Name, const FLinearColor& Value);

	UPROPERTY(Transient)
	TArray<FJRPGVFXInstance> Instances;

	/** O dono das malhas e luzes dos efeitos (um por mundo, criado no primeiro uso). */
	UPROPERTY(Transient)
	TObjectPtr<AActor> Host;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> FreeMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPointLightComponent>> FreeLights;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UPostProcessComponent>> FreePostProcesses;

	/** Quantas camadas PostProcess pediram a Custom Depth de cada malha (só as que o subsistema ligou). */
	TMap<TWeakObjectPtr<UPrimitiveComponent>, int32> CustomDepthUsers;

	/** Aleatoriedade só de aparência. Nunca o Rng da luta. */
	FRandomStream CosmeticRng;

	int32 NextId = 1;
};
