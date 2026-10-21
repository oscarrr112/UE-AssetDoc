// Copyright 2026 Oscar Peng. Licensed under the Apache License, Version 2.0.

// The Director Blueprint of a Level Sequence (design §3.6): the hosted-Blueprint resolver the Blueprint providers use
// (the profile's Director region is the Blueprint profile's members and graphs, edited through the sequence's handles),
// its creation and removal (seq.edit.v1 director.create / director.remove), and the values that call it: event keys and
// event sections ({Endpoint, BoundObjectPin?, Payload?}, FMovieSceneEventUtils::SetEndpoint as the event's endpoint
// picker) and director bindings ({Endpoint, ResolveParamsPin?, Payload?}, FMovieSceneDynamicBindingUtils::SetEndpoint).

#include "AssetDocumentSequencerEdit.h"

#include "BlueprintActionDatabase.h"
#include "Channels/MovieSceneEvent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "LevelSequenceDirector.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneDynamicBinding.h"
#include "MovieSceneDynamicBindingUtils.h"
#include "MovieSceneEventUtils.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSequenceEditor.h"
#include "MovieSceneTrack.h"
#include "Sections/MovieSceneEventSectionBase.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace UE::AssetDocument::Graph::Sequencer
{
namespace
{
// A field's path below Path ("" = the reader's own value).
FString Under(const FString& Path, const FString& Key)
{
	return Path.IsEmpty() ? Escape(Key) : Path + TEXT("/") + Escape(Key);
}

FString JoinKeys(std::initializer_list<const TCHAR*> Keys)
{
	FString Out;
	for (const TCHAR* Key : Keys)
	{
		Out += Out.IsEmpty() ? FString(Key) : FString(TEXT(", ")) + Key;
	}
	return Out;
}

const UMovieSceneSequence* SequenceOf(const UObject& Inside)
{
	if (const UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(&Inside))
	{
		return Sequence;
	}
	return Inside.GetTypedOuter<UMovieSceneSequence>();
}

// The data pins of an endpoint node (a function entry's or custom event's outputs: the function's inputs).
TArray<UEdGraphPin*> DataPins(const UK2Node& Node)
{
	TArray<UEdGraphPin*> Pins;
	for (UEdGraphPin* Pin : Node.Pins)
	{
		if (Pin != nullptr && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& !Pin->bHidden && Pin->PinName != UEdGraphSchema_K2::PN_DelegateEntry)
		{
			Pins.Add(Pin);
		}
	}
	return Pins;
}

FString PinNames(const UK2Node& Node, const UEdGraphPin* Except = nullptr)
{
	TArray<FString> Names;
	for (const UEdGraphPin* Pin : DataPins(Node))
	{
		if (Pin != Except)
		{
			Names.Add(Pin->PinName.ToString());
		}
	}
	return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
}

UEdGraphPin* FindDataPin(const UK2Node& Node, const FString& Name)
{
	for (UEdGraphPin* Pin : DataPins(Node))
	{
		if (Pin->PinName.ToString() == Name)
		{
			return Pin;
		}
	}
	return nullptr;
}

bool IsObjectPin(const UEdGraphPin& Pin)
{
	const FName Category = Pin.PinType.PinCategory;
	return Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Interface
		|| Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass;
}

// The endpoints an event key or director binding can name (function graphs; custom events unless FunctionsOnly).
TArray<UK2Node*> Endpoints(const UBlueprint& Director, bool bFunctionsOnly)
{
	TArray<UK2Node*> Out;
	for (const UEdGraph* Graph : Director.FunctionGraphs)
	{
		for (UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				Out.Add(Entry);
			}
		}
	}
	if (!bFunctionsOnly)
	{
		for (const UEdGraph* Graph : Director.UbergraphPages)
		{
			for (UEdGraphNode* Node : Graph != nullptr ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
			{
				if (UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node))
				{
					Out.Add(Event);
				}
			}
		}
	}
	return Out;
}

bool FindEndpoint(const FValue& Value, FDecodeContext& Context, bool bFunctionsOnly, UBlueprint*& OutDirector, UK2Node*& OutNode)
{
	const FString Path = Under(Context.Path, TEXT("Endpoint"));
	if (Value.Kind != EAssetDocumentRuntimeValueKind::String || Value.StringValue.IsEmpty())
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Endpoint is the name of a Director function (or custom event)."), Path);
	}
	OutDirector = Context.Owner != nullptr ? FindDirectorBlueprint(*Context.Owner) : nullptr;
	if (OutDirector == nullptr)
	{
		return Context.Reader.Fail(TEXT("Sequencer.DirectorRequired"), FString::Printf(TEXT("Endpoint '%s' is a function of the Director Blueprint, "
			"and this sequence has none: list Body.Director with that function (FunctionGraphs) or custom event (UbergraphPages)."),
			*Value.StringValue), Path);
	}
	TArray<FString> Names;
	for (UK2Node* Node : Endpoints(*OutDirector, bFunctionsOnly))
	{
		const FString Name = EndpointName(Node);
		if (Name == Value.StringValue)
		{
			OutNode = Node;
			return true;
		}
		Names.Add(Name);
	}
	return Context.Reader.Fail(TEXT("Sequencer.UnknownEventEndpoint"), FString::Printf(TEXT("The Director Blueprint has no %s '%s'. %s: %s."),
		bFunctionsOnly ? TEXT("function") : TEXT("function or custom event"), *Value.StringValue,
		bFunctionsOnly ? TEXT("Functions") : TEXT("Endpoints"), Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", "))), Path);
}

bool SameValueType(const FEdGraphPinType& A, const FEdGraphPinType& B)
{
	return A.PinCategory == B.PinCategory && A.PinSubCategory == B.PinSubCategory
		&& A.PinSubCategoryObject == B.PinSubCategoryObject && A.ContainerType == B.ContainerType;
}

// Whether Text is a value of the endpoint input Pin, read as the endpoint's Details read a payload value
// (FMovieSceneDirectorBlueprintEndpointCustomization: FBlueprintEditorUtils::PropertyValueFromString into the
// endpoint function's parameter). Before the Director compiles a new or retyped input (no parameter of the pin's
// type yet), the K2 schema's default-value check of the pin's type (UEdGraphSchema_K2::DefaultValueSimpleValidation).
bool PayloadParses(const UK2Node& Node, const UEdGraphPin& Pin, const FString& Text, FString& OutReason)
{
	const UFunction* Signature = nullptr;
	if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(&Node))
	{
		Signature = Entry->FindSignatureFunction();
	}
	else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(&Node))
	{
		Signature = Event->FindEventSignatureFunction();
	}
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	const FProperty* Parameter = Signature != nullptr ? FindFProperty<FProperty>(Signature, Pin.PinName) : nullptr;
	FEdGraphPinType ParameterType;
	if (Parameter != nullptr && Schema->ConvertPropertyToPinType(Parameter, ParameterType) && SameValueType(ParameterType, Pin.PinType))
	{
		FStructOnScope Parameters(Signature);
		if (FBlueprintEditorUtils::PropertyValueFromString(Parameter, Text, Parameters.GetStructMemory()))
		{
			return true;
		}
		OutReason = TEXT("it does not import as that type");
		return false;
	}
	FString Value;
	TObjectPtr<UObject> Object;
	FText TextValue;
	Schema->GetPinDefaultValuesFromString(Pin.PinType, nullptr, Text, Value, Object, TextValue);
	return Schema->DefaultValueSimpleValidation(Pin.PinType, Pin.PinName, Value, Object, TextValue, &OutReason);
}

// Payload {pin: text} onto PayloadVariables, as the endpoint's Details write it (text; ObjectValue too when the text names
// an object, FMovieSceneDirectorBlueprintEndpointCustomization::OnPayloadVariableChanged).
template <typename FVariable>
bool DecodePayload(const FValue* Payload, const UK2Node& Node, const UEdGraphPin* Exclude, const TCHAR* ExcludeKey, TMap<FName, FVariable>& Out,
	FDecodeContext& Context)
{
	Out.Reset();
	if (Payload == nullptr)
	{
		return true;
	}
	const FString Path = Under(Context.Path, TEXT("Payload"));
	if (Payload->Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Payload is an object of endpoint input name -> value text."), Path);
	}
	for (int32 Index = 0; Index < Payload->ObjectKeys.Num(); ++Index)
	{
		const FString& Name = Payload->ObjectKeys[Index];
		const FValue& Text = Payload->Elements[Index];
		const UEdGraphPin* Pin = FindDataPin(Node, Name);
		if (Pin == nullptr || Pin == Exclude)
		{
			return Context.Reader.Fail(TEXT("Sequencer.UnknownEventPin"), FString::Printf(TEXT("'%s' is not an input of %s that takes a payload%s. "
				"Inputs: %s."), *Name, *EndpointName(&Node), Pin != nullptr ? *FString::Printf(TEXT(" (it is the %s)"), ExcludeKey) : TEXT(""),
				*PinNames(Node, Exclude)), Under(Path, Name));
		}
		if (Text.Kind != EAssetDocumentRuntimeValueKind::String)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("A payload value is the input's value as text "
				"(\"1.5\", \"true\", an object path); got a non-string for '%s'."), *Name), Under(Path, Name));
		}
		FVariable Variable;
		Variable.Value = Text.StringValue;
		if (IsObjectPin(*Pin) && FPackageName::IsValidPath(Text.StringValue))
		{
			UObject* Object = FindObject<UObject>(nullptr, *Text.StringValue);
			if (Object == nullptr)
			{
				Object = LoadObject<UObject>(nullptr, *Text.StringValue, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			if (Object != nullptr)
			{
				Variable.ObjectValue = FSoftObjectPath(Object);
				Variable.Value = Object->GetPathName();
			}
		}
		// An empty value is what the endpoint picker stores for an input whose pin has no default text (the Details then
		// show the type's default); any other text must be a value of the input's type.
		FString Reason;
		if (!Variable.Value.IsEmpty() && !PayloadParses(Node, *Pin, Variable.Value, Reason))
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidPayload"), FString::Printf(TEXT("'%s' is not a value of input '%s' of %s, "
				"which is %s (%s). Write the value as the endpoint's Details would show it (\"2.5\", \"true\", an enum entry name, "
				"an object path)."), *Text.StringValue, *Name, *EndpointName(&Node), *UEdGraphSchema_K2::TypeToText(Pin->PinType).ToString(),
				Reason.IsEmpty() ? TEXT("it does not parse as that type") : *Reason), Under(Path, Name));
		}
		Out.Add(Pin->PinName, MoveTemp(Variable));
	}
	return true;
}

template <typename FVariable>
void EncodePayload(const TMap<FName, FVariable>& Variables, FObjectBuilder& Object)
{
	if (Variables.IsEmpty())
	{
		return;
	}
	TArray<FName> Pins;
	Variables.GetKeys(Pins);
	Pins.Sort(FNameLexicalLess());
	FObjectBuilder Payload;
	for (const FName& Pin : Pins)
	{
		const FVariable& Variable = Variables[Pin];
		Payload.AddString(Pin.ToString(), Variable.ObjectValue.IsValid() ? Variable.ObjectValue.ToString() : Variable.Value);
	}
	Object.Add(TEXT("Payload"), Payload.Build());
}

bool CheckValueFields(const FValue& Value, FDecodeContext& Context, std::initializer_list<const TCHAR*> Allowed, const TCHAR* What)
{
	if (Value.Kind != EAssetDocumentRuntimeValueKind::Object)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), FString::Printf(TEXT("%s is an object {%s}."), What,
			*JoinKeys(Allowed)), Context.Path);
	}
	for (const FString& Key : Value.ObjectKeys)
	{
		bool bKnown = false;
		for (const TCHAR* Name : Allowed)
		{
			bKnown |= Key == Name;
		}
		if (!bKnown)
		{
			return Context.Reader.Fail(TEXT("Graph.UnknownField"), FString::Printf(TEXT("%s takes %s; '%s' is not one of them."), What,
				*JoinKeys(Allowed), *Key), Under(Context.Path, Key));
		}
	}
	return true;
}

// Moves an object out of the sequence package (the removed director, its classes and their defaults), as a deleted
// Blueprint's generated objects leave their package.
void MoveToTransient(UObject* Object)
{
	if (Object == nullptr || Object->GetOutermost() == GetTransientPackage())
	{
		return;
	}
	Object->Modify();
	const FName Name = MakeUniqueObjectName(GetTransientPackage(), Object->GetClass(), FName(*(TEXT("REMOVED_") + Object->GetName())));
	Object->Rename(*Name.ToString(), GetTransientPackage(), REN_DontCreateRedirectors);
	Object->ClearFlags(RF_Public | RF_Standalone);
}

// Objects of the sequence package that are no longer part of the sequence (a track, section or binding the same apply
// removed stays in the package until garbage collection) may still call the Director; saving them would reference
// the removed Director's objects. Their calls are cleared, as the endpoint pickers' "None" does.
void ReleaseDetachedCallers(const ULevelSequence& Level, const UBlueprint& Blueprint)
{
	const UClass* Generated = Blueprint.GeneratedClass;
	auto Calls = [&Blueprint, Generated](const UObject* Object)
	{
		return Object != nullptr && (Object->IsIn(&Blueprint) || (Generated != nullptr && Object->IsIn(Generated)));
	};
	ForEachObjectWithPackage(Level.GetOutermost(), [&Blueprint, &Calls](UObject* Object)
	{
		if (Object->IsIn(&Blueprint))
		{
			return true;
		}
		if (UMovieSceneEventSectionBase* Section = Cast<UMovieSceneEventSectionBase>(Object))
		{
			for (FMovieSceneEvent& Event : Section->GetAllEntryPoints())
			{
				if (Calls(Event.WeakEndpoint.Get()) || Calls(Event.Ptrs.Function))
				{
					Section->Modify();
					Event.WeakEndpoint = nullptr;
					Event.Ptrs = FMovieSceneEventPtrs();
					Event.CompiledFunctionName = NAME_None;
				}
			}
		}
		for (TFieldIterator<FStructProperty> It(Object->GetClass()); It; ++It)
		{
			if (It->Struct != FMovieSceneDynamicBinding::StaticStruct())
			{
				continue;
			}
			FMovieSceneDynamicBinding* Binding = It->ContainerPtrToValuePtr<FMovieSceneDynamicBinding>(Object);
			if (Calls(Binding->WeakEndpoint.Get()) || Calls(Binding->Function))
			{
				Object->Modify();
				Binding->WeakEndpoint = nullptr;
				Binding->Function = nullptr;
				Binding->ResolveParamsProperty = TFieldPath<FProperty>();
				Binding->CompiledFunctionName = NAME_None;
			}
		}
		return true;
	});
}
} // namespace

UBlueprint* FindDirectorBlueprint(const UObject& Inside)
{
	const ULevelSequence* Level = Cast<ULevelSequence>(SequenceOf(Inside));
	return Level != nullptr ? Level->GetDirectorBlueprint() : nullptr;
}

UBlueprint* ResolveHostedDirector(UObject& Host)
{
	const ULevelSequence* Level = Cast<ULevelSequence>(&Host);
	return Level != nullptr ? Level->GetDirectorBlueprint() : nullptr;
}

FValue DescribeDirectorHeader(const UMovieSceneSequence& Sequence)
{
	const UBlueprint* Blueprint = FindDirectorBlueprint(Sequence);
	if (Blueprint == nullptr)
	{
		return MakeNull();
	}
	FObjectBuilder Director;
	Director.AddString(TEXT("Blueprint"), Blueprint->GetPathName());
	const UClass* Generated = Blueprint->GeneratedClass;
	Director.AddString(TEXT("Class"), Generated != nullptr ? Generated->GetPathName()
		: Blueprint->GetOutermost()->GetName() + TEXT(".") + Blueprint->GetName() + TEXT("_C"));
	return Director.Build();
}

bool DecodeEvent(const FValue& Value, FMovieSceneEvent& Event, FDecodeContext& Context)
{
	if (!CheckValueFields(Value, Context, {TEXT("Endpoint"), TEXT("BoundObjectPin"), TEXT("Payload")}, TEXT("An event")))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	UMovieSceneEventSectionBase* Section = Cast<UMovieSceneEventSectionBase>(Context.Owner);
	const FValue* Endpoint = FindField(Value, TEXT("Endpoint"));
	if (Endpoint == nullptr)
	{
		if (Value.ObjectKeys.Num() > 0)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("BoundObjectPin and Payload are inputs of an Endpoint; list the Endpoint."),
				Context.Path);
		}
		if (Section != nullptr && Event.WeakEndpoint.IsValid())
		{
			FMovieSceneEventUtils::SetEndpoint(&Event, Section, nullptr, nullptr);
		}
		Event = FMovieSceneEvent();
		return true;
	}
	if (Section == nullptr)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("An event endpoint is set on an event section's events."), Context.Path);
	}
	UBlueprint* Director = nullptr;
	UK2Node* Node = nullptr;
	if (!FindEndpoint(*Endpoint, Context, false, Director, Node))
	{
		return false;
	}
	UEdGraphPin* BoundPin = nullptr;
	if (const FValue* Pin = FindField(Value, TEXT("BoundObjectPin")))
	{
		BoundPin = Pin->Kind == EAssetDocumentRuntimeValueKind::String ? FindDataPin(*Node, Pin->StringValue) : nullptr;
		if (BoundPin == nullptr || !IsObjectPin(*BoundPin))
		{
			return Context.Reader.Fail(TEXT("Sequencer.UnknownEventPin"), FString::Printf(TEXT("BoundObjectPin is the object input of %s that "
				"receives the bound object; %s. Inputs: %s."), *EndpointName(Node), BoundPin == nullptr ? TEXT("there is no such input")
				: TEXT("that input is not an object"), *PinNames(*Node)), Under(Context.Path, TEXT("BoundObjectPin")));
		}
	}
	FMovieSceneEventUtils::BindEventSectionToBlueprint(Section, Director);
	FMovieSceneEventUtils::SetEndpoint(&Event, Section, Node, BoundPin);
	return DecodePayload(FindField(Value, TEXT("Payload")), *Node, BoundPin, TEXT("BoundObjectPin"), Event.PayloadVariables, Context);
#else
	return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Event endpoints are editor data."), Context.Path);
#endif
}

FValue EncodeDynamicBinding(const FMovieSceneDynamicBinding& Binding, FEncodeContext& Context)
{
	FObjectBuilder Object;
#if WITH_EDITORONLY_DATA
	// The resolve parameters pin and payload are inputs of the endpoint (as for event keys).
	const UObject* Endpoint = LiveEndpoint(Binding.WeakEndpoint);
	if (Endpoint == nullptr)
	{
		return Object.Build();
	}
	Object.AddString(TEXT("Endpoint"), EndpointName(Endpoint));
	if (!Binding.ResolveParamsPinName.IsNone())
	{
		Object.Add(TEXT("ResolveParamsPin"), MakeName(Binding.ResolveParamsPinName));
	}
	EncodePayload(Binding.PayloadVariables, Object);
#endif
	return Object.Build();
}

bool DecodeDynamicBinding(const FValue& Value, FMovieSceneDynamicBinding& Binding, FDecodeContext& Context)
{
	if (!CheckValueFields(Value, Context, {TEXT("Endpoint"), TEXT("ResolveParamsPin"), TEXT("Payload")}, TEXT("A director binding")))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	UMovieSceneSequence* Sequence = Context.Owner != nullptr ? const_cast<UMovieSceneSequence*>(SequenceOf(*Context.Owner)) : nullptr;
	UMovieScene* MovieScene = Sequence != nullptr ? Sequence->GetMovieScene() : nullptr;
	const FValue* Endpoint = FindField(Value, TEXT("Endpoint"));
	if (Endpoint == nullptr)
	{
		if (Value.ObjectKeys.Num() > 0)
		{
			return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("ResolveParamsPin and Payload are inputs of an Endpoint; list the Endpoint."),
				Context.Path);
		}
		if (MovieScene != nullptr && Binding.WeakEndpoint.IsValid())
		{
			FMovieSceneDynamicBindingUtils::SetEndpoint(MovieScene, &Binding, nullptr);
		}
		Binding = FMovieSceneDynamicBinding();
		return true;
	}
	if (MovieScene == nullptr)
	{
		return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("A director binding's endpoint is set on a binding of a sequence."), Context.Path);
	}
	UBlueprint* Director = nullptr;
	UK2Node* Node = nullptr;
	if (!FindEndpoint(*Endpoint, Context, true, Director, Node))
	{
		return false;
	}
	UEdGraphPin* ResolvePin = nullptr;
	Binding.ResolveParamsPinName = NAME_None;
	if (const FValue* Pin = FindField(Value, TEXT("ResolveParamsPin")))
	{
		ResolvePin = Pin->Kind == EAssetDocumentRuntimeValueKind::String ? FindDataPin(*Node, Pin->StringValue) : nullptr;
		if (ResolvePin == nullptr)
		{
			return Context.Reader.Fail(TEXT("Sequencer.UnknownEventPin"), FString::Printf(TEXT("ResolveParamsPin is the input of %s that receives "
				"the resolve parameters (FMovieSceneDynamicBindingResolveParams). Inputs: %s."), *EndpointName(Node), *PinNames(*Node)),
				Under(Context.Path, TEXT("ResolveParamsPin")));
		}
		Binding.ResolveParamsPinName = ResolvePin->PinName;
	}
	FMovieSceneDynamicBindingUtils::EnsureBlueprintExtensionCreated(Sequence, Director);
	FMovieSceneDynamicBindingUtils::SetEndpoint(MovieScene, &Binding, Node);
	return DecodePayload(FindField(Value, TEXT("Payload")), *Node, ResolvePin, TEXT("ResolveParamsPin"), Binding.PayloadVariables, Context);
#else
	return Context.Reader.Fail(TEXT("Sequencer.InvalidValue"), TEXT("Director binding endpoints are editor data."), Context.Path);
#endif
}

TArray<FString> DirectorUsers(const UMovieSceneSequence& Sequence)
{
	TArray<FString> Users;
#if WITH_EDITORONLY_DATA
	FSequenceModel Model;
	FString Error;
	if (!BuildModel(Sequence, Model, Error))
	{
		return Users;
	}
	for (const FTrackInfo& Track : Model.Tracks)
	{
		const TArray<UMovieSceneSection*>& Sections = Track.Track->GetAllSections();
		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			UMovieSceneEventSectionBase* Section = Cast<UMovieSceneEventSectionBase>(Sections[Index]);
			if (Section == nullptr)
			{
				continue;
			}
			for (const FMovieSceneEvent& Event : Section->GetAllEntryPoints())
			{
				if (LiveEndpoint(Event.WeakEndpoint) != nullptr)
				{
					Users.Add(FString::Printf(TEXT("%s/Sections/%d"), *Track.DocumentPath, Index));
					break;
				}
			}
		}
	}
	const_cast<UMovieSceneSequence&>(Sequence).IterateDynamicBindings([&Users, &Model](const FGuid& Guid, FMovieSceneDynamicBinding& Binding)
	{
		if (LiveEndpoint(Binding.WeakEndpoint) != nullptr)
		{
			const FBindingInfo* Info = Model.FindBinding(Guid);
			Users.AddUnique(Info != nullptr ? Info->DocumentPath : Guid.ToString(EGuidFormats::Digits));
		}
	});
#endif
	return Users;
}
} // namespace UE::AssetDocument::Graph::Sequencer

namespace UE::AssetDocument::Graph::Sequencer::Edit
{
bool OpDirectorCreate(FEdit& Edit, FReader& Op)
{
	ULevelSequence* Level = Cast<ULevelSequence>(&Edit.Sequence);
	if (Level == nullptr)
	{
		return Op.Fail(TEXT("Sequencer.DirectorUnsupported"), TEXT("Only a Level Sequence has a Director Blueprint."));
	}
	UBlueprint* Blueprint = Level->GetDirectorBlueprint();
	if (Blueprint == nullptr)
	{
		Level->Modify();
		// Sequencer's "Open Director Blueprint" (FMovieSceneSequenceEditor_LevelSequence::CreateBlueprintForSequence).
		if (FMovieSceneSequenceEditor* SequenceEditor = FMovieSceneSequenceEditor::Find(Level))
		{
			Blueprint = SequenceEditor->GetOrCreateDirectorBlueprint(Level);
		}
		if (Blueprint == nullptr)
		{
			Blueprint = FKismetEditorUtilities::CreateBlueprint(ULevelSequenceDirector::StaticClass(), Level, FName(*Level->GetDirectorBlueprintName()),
				BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
			if (Blueprint != nullptr)
			{
				Blueprint->ClearFlags(RF_Standalone);
				Level->SetDirectorBlueprint(Blueprint);
			}
		}
		if (Blueprint == nullptr)
		{
			return Op.Fail(TEXT("Sequencer.DirectorCreateFailed"), TEXT("The Director Blueprint could not be created."));
		}
		Blueprint->SetFlags(RF_Transactional);
		Edit.bChanged = true;
	}
	FObjectBuilder Row;
	Row.AddInt(TEXT("op"), Edit.CurrentOp);
	Row.AddString(TEXT("object"), Blueprint->GetPathName());
	Row.Add(TEXT("director"), DescribeDirectorHeader(*Level));
	Edit.Created.Add(Row.Build());
	return true;
}

bool OpDirectorRemove(FEdit& Edit, FReader& Op)
{
	ULevelSequence* Level = Cast<ULevelSequence>(&Edit.Sequence);
	UBlueprint* Blueprint = Level != nullptr ? Level->GetDirectorBlueprint() : nullptr;
	if (Blueprint == nullptr)
	{
		return true;
	}
	const TArray<FString> Users = DirectorUsers(*Level);
	if (!Users.IsEmpty())
	{
		return Op.Fail(TEXT("Sequencer.DirectorInUse"), FString::Printf(TEXT("The Director Blueprint is called by %s; remove those event endpoints "
			"and director bindings (or keep the Director)."), *FString::Join(Users, TEXT(", "))));
	}
	ReleaseDetachedCallers(*Level, *Blueprint);
	Level->Modify();
	Blueprint->Modify();
	Level->SetDirectorBlueprint(nullptr);
	UClass* Generated = Blueprint->GeneratedClass;
	UClass* Skeleton = Blueprint->SkeletonGeneratedClass;
	// As for a deleted Blueprint asset (the action database's pending-delete handler): its node actions leave the
	// Blueprint editors' menus.
	FBlueprintActionDatabase& Actions = FBlueprintActionDatabase::Get();
	Actions.ClearAssetActions(Blueprint);
	for (UClass* Class : {Generated, Skeleton})
	{
		if (Class != nullptr)
		{
			Actions.ClearAssetActions(Class);
		}
	}
	for (UClass* Class : {Generated, Skeleton})
	{
		if (Class != nullptr)
		{
			MoveToTransient(Class->GetDefaultObject(false));
			MoveToTransient(Class);
		}
	}
	MoveToTransient(Blueprint);
	Edit.bChanged = true;
	return true;
}
} // namespace UE::AssetDocument::Graph::Sequencer::Edit
