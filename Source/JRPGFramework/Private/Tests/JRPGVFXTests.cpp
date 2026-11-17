// Testes do VFX: só a parte que é dado puro (as faixas e a duração da receita), sem mapa nem assets.
// Session Frontend › Automation, filtro "JRPG.VFX".

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VFX/JRPGVFXRecipe.h"
#include "VFX/JRPGVFXSubsystem.h"
#include "VFX/JRPGVFXTypes.h"
#include "UObject/Package.h"

#define JRPG_VFX_TEST_FLAGS (EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJRPGVFXTracksTest, "JRPG.VFX.Tracks", JRPG_VFX_TEST_FLAGS)
bool FJRPGVFXTracksTest::RunTest(const FString& Parameters)
{
	// Todo Ease começa em 0; o Pulse volta a 0 no fim, os outros chegam a 1
	for (const EJRPGVFXEase Ease : { EJRPGVFXEase::Linear, EJRPGVFXEase::EaseIn, EJRPGVFXEase::EaseOut, EJRPGVFXEase::EaseInOut, EJRPGVFXEase::Pulse })
	{
		TestEqual(TEXT("Ease em 0"), JRPGVFX::EvaluateEase(Ease, 0.0f), 0.0f, 1e-4f);
		TestEqual(TEXT("Ease em 1"), JRPGVFX::EvaluateEase(Ease, 1.0f), Ease == EJRPGVFXEase::Pulse ? 0.0f : 1.0f, 1e-4f);
	}
	TestEqual(TEXT("Pulse no meio"), JRPGVFX::EvaluateEase(EJRPGVFXEase::Pulse, 0.5f), 1.0f, 1e-4f);
	TestEqual(TEXT("Fora de 0..1 é preso"), JRPGVFX::EvaluateEase(EJRPGVFXEase::Linear, 3.0f), 1.0f, 1e-4f);

	FJRPGVFXScalarTrack Scalar;
	Scalar.From = 2.0f;
	Scalar.To = 6.0f;
	TestEqual(TEXT("Escalar linear no meio"), Scalar.Evaluate(0.5f), 4.0f, 1e-4f);
	Scalar.Ease = EJRPGVFXEase::Pulse;
	TestEqual(TEXT("Escalar em Pulse volta ao From"), Scalar.Evaluate(1.0f), 2.0f, 1e-3f);

	// Chaves em segundos: sobe, segura, desce; parado antes da primeira e depois da última
	FJRPGVFXScalarTrack Keyed;
	Keyed.From = 9.0f;   // ignorado com chaves
	Keyed.To = 9.0f;
	Keyed.Ease = EJRPGVFXEase::Linear;
	Keyed.Keys = { { 0.5f, 0.0f }, { 1.5f, 1.0f }, { 3.0f, 1.0f }, { 4.0f, 0.0f } };
	TestEqual(TEXT("Chaves: antes da primeira"), Keyed.EvaluateAt(0.9f, 0.1f), 0.0f, 1e-4f);
	TestEqual(TEXT("Chaves: no meio da subida"), Keyed.EvaluateAt(0.0f, 1.0f), 0.5f, 1e-4f);
	TestEqual(TEXT("Chaves: segurando"), Keyed.EvaluateAt(0.0f, 2.0f), 1.0f, 1e-4f);
	TestEqual(TEXT("Chaves: no meio da descida"), Keyed.EvaluateAt(0.0f, 3.5f), 0.5f, 1e-4f);
	TestEqual(TEXT("Chaves: depois da última"), Keyed.EvaluateAt(0.0f, 60.0f), 0.0f, 1e-4f);
	Keyed.Ease = EJRPGVFXEase::EaseIn;
	TestEqual(TEXT("Chaves: o Ease vale em cada par"), Keyed.EvaluateAt(0.0f, 1.0f), 0.25f, 1e-4f);

	FJRPGVFXColorTrack Color;
	Color.From = FLinearColor::Black;
	Color.To = FLinearColor::White;
	Color.bTintWithContextColor = true;
	const FLinearColor Tinted = Color.Evaluate(1.0f, FLinearColor(1.0f, 0.5f, 0.0f, 1.0f));
	TestEqual(TEXT("Cor com tint do contexto (G)"), Tinted.G, 0.5f, 1e-4f);
	TestEqual(TEXT("Cor com tint do contexto (B)"), Tinted.B, 0.0f, 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJRPGVFXRecipeLengthTest, "JRPG.VFX.RecipeLength", JRPG_VFX_TEST_FLAGS)
bool FJRPGVFXRecipeLengthTest::RunTest(const FString& Parameters)
{
	UJRPGVFXRecipe* Recipe = NewObject<UJRPGVFXRecipe>(GetTransientPackage());
	TestTrue(TEXT("Receita vazia tem duração mínima, nunca 0"), Recipe->GetLength() > 0.0f);

	FJRPGVFXLayer Burst;
	Burst.StartTime = 0.2f;
	Burst.Duration = 0.5f;
	FJRPGVFXLayer Open;   // sem duração própria: não estica o efeito além do começo dela
	Open.StartTime = 1.0f;
	Recipe->Layers = { Burst, Open };
	TestEqual(TEXT("Duração = o fim da última camada"), Recipe->GetLength(), 1.0f, 1e-4f);

	Recipe->Layers[0].Duration = 2.0f;
	TestEqual(TEXT("A camada mais longa manda"), Recipe->GetLength(), 2.2f, 1e-4f);

	Recipe->Duration = 3.0f;
	TestEqual(TEXT("Duration explícito vence"), Recipe->GetLength(), 3.0f, 1e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJRPGVFXFloatParamTest, "JRPG.VFX.FloatParam", JRPG_VFX_TEST_FLAGS)
bool FJRPGVFXFloatParamTest::RunTest(const FString& Parameters)
{
	// SetEffectFloat: espera o Delay e vai de From a To em BlendTime
	FJRPGVFXFloatParam Blend;
	Blend.From = 1.0f;
	Blend.To = 3.0f;
	Blend.Delay = 0.5f;
	Blend.BlendTime = 1.0f;
	Blend.Elapsed = 0.2f;
	TestEqual(TEXT("Blend: antes do Delay"), Blend.Evaluate(), 1.0f, 1e-4f);
	Blend.Elapsed = 1.0f;
	TestEqual(TEXT("Blend: no meio"), Blend.Evaluate(), 2.0f, 1e-4f);
	Blend.Elapsed = 9.0f;
	TestEqual(TEXT("Blend: no fim"), Blend.Evaluate(), 3.0f, 1e-4f);

	// PulseEffectFloat: sobe ao Peak, volta ao Rest e fica
	FJRPGVFXFloatParam Pulse;
	Pulse.From = 0.0f;
	Pulse.To = 1.3f;
	Pulse.BlendTime = 0.2f;
	Pulse.bPulse = true;
	Pulse.FallTime = 0.4f;
	Pulse.Rest = 1.0f;
	Pulse.Elapsed = 0.0f;
	TestEqual(TEXT("Pulso: começa no From"), Pulse.Evaluate(), 0.0f, 1e-4f);
	Pulse.Elapsed = 0.2f;
	TestEqual(TEXT("Pulso: no pico"), Pulse.Evaluate(), 1.3f, 1e-4f);
	Pulse.Elapsed = 0.4f;
	TestEqual(TEXT("Pulso: no meio da volta"), Pulse.Evaluate(), 1.15f, 1e-4f);
	Pulse.Elapsed = 5.0f;
	TestEqual(TEXT("Pulso: fica no Rest"), Pulse.Evaluate(), 1.0f, 1e-4f);
	Pulse.BlendTime = 0.0f;
	Pulse.Elapsed = 0.0f;
	TestEqual(TEXT("Pulso sem subida: começa no pico"), Pulse.Evaluate(), 1.3f, 1e-4f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
