// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.
// SPDX-License-Identifier: Apache-2.0
#include "Provider/Montage/MontageStructuralRollback.h"

namespace AssetDocumentMontage
{
FCompositeSection BuildMontageSection(const FCompositeSection* Preimage, FName Name, FName Next)
{
 FCompositeSection Section = Preimage ? *Preimage : FCompositeSection();
 Section.SectionName = Name; Section.NextSectionName = Next;
 return Section;
}
void PublishMontageSections(UAnimMontage& Target, const TArray<FCompositeSection>& Sections,
 TConstArrayView<FLinkPlacement> Links)
{
 check(Sections.Num() == Links.Num());
 // AD.ANIMATION.MONTAGE.SECTIONS-PUBLISH.v1: sole forward assignment; no implicit Add next-link.
 Target.CompositeSections = Sections;
 for (int32 Index = 0; Index < Links.Num(); ++Index)
  SetLinkPlacement(Target.CompositeSections[Index], Target, Links[Index]);
}
}
