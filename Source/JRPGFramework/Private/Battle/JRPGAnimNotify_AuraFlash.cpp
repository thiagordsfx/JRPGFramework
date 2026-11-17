#include "Battle/JRPGAnimNotify_AuraFlash.h"
#include "Animation/AnimSequenceBase.h"

bool UJRPGAnimNotify_AuraFlash::FindFlashTime(const UAnimSequenceBase* Clip, float& OutSeconds)
{
	bool bFound = false;
	if (Clip)
	{
		for (const FAnimNotifyEvent& Event : Clip->Notifies)
		{
			if (Event.Notify && Event.Notify->IsA<UJRPGAnimNotify_AuraFlash>() && (!bFound || Event.GetTriggerTime() < OutSeconds))
			{
				OutSeconds = Event.GetTriggerTime();
				bFound = true;
			}
		}
	}
	return bFound;
}
