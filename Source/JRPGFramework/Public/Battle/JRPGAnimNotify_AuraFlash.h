#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "JRPGAnimNotify_AuraFlash.generated.h"

class UAnimSequenceBase;

/**
 * UJRPGAnimNotify_AuraFlash
 * Marca no clip do SpecialStarter o quadro em que a absorção termina e a aura da Art pisca e cresce (AJRPGBattlePawn::
 * StartArtAuraVFX). Como o UJRPGAnimNotify_Hit, NÃO faz nada sozinho: o pawn lê o instante do asset quando o passo entra,
 * converte em quadro lógico (com as voltas do Loop Window, se o flash vem depois dele) e estica a absorção para acabar ali.
 * Um clip sem este notify pisca no fim do passo. Trilha sugerida: "VFX".
 */
UCLASS(meta = (DisplayName = "Art Aura Flash"))
class JRPGFRAMEWORK_API UJRPGAnimNotify_AuraFlash : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual FString GetNotifyName_Implementation() const override { return TEXT("Aura Flash"); }

	/** O instante do primeiro Art Aura Flash do clip, em segundos do asset. Falso se o clip não tem. */
	static bool FindFlashTime(const UAnimSequenceBase* Clip, float& OutSeconds);
};
