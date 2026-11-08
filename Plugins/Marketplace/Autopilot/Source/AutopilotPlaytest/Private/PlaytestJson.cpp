#include "PlaytestJson.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/EnumProperty.h"
#include "Misc/EngineVersionComparison.h"
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
#include "UObject/StrProperty.h"
#endif
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/Class.h"
#include "UObject/Package.h"

// A set's or map's element by iterator: before 5.4 the helpers take the iterator's index
#if UE_VERSION_OLDER_THAN(5, 4, 0)
#define PLAYTEST_CONTAINER_INDEX(It) (*(It))
#else
#define PLAYTEST_CONTAINER_INDEX(It) (It)
#endif

namespace PlaytestJson
{
namespace
{
	FString EnumText(const UEnum* Enum, int64 Value)
	{
		if (!Enum || !Enum->IsValidEnumValue(Value))
		{
			return FString();
		}
		FString Text = Enum->GetDisplayNameTextByValue(Value).ToString();
		if (Text.IsEmpty())
		{
			Text = Enum->GetAuthoredNameStringByValue(Value);
		}
		return Text;
	}

	bool EnumValue(const UEnum* Enum, const TSharedPtr<FJsonValue>& Json, int64& OutValue)
	{
		double Number = 0;
		if (Json->Type == EJson::Number && Json->TryGetNumber(Number))
		{
			OutValue = (int64)Number;
			return true;
		}
		FString Text;
		if (!Json->TryGetString(Text))
		{
			return false;
		}
		if (Text.IsNumeric())
		{
			OutValue = FCString::Atoi64(*Text);
			return true;
		}
		int64 Value = Enum->GetValueByNameString(Text);
		if (Value != INDEX_NONE)
		{
			OutValue = Value;
			return true;
		}
		for (int32 i = 0; i < Enum->NumEnums(); ++i)
		{
			if (Enum->GetDisplayNameTextByIndex(i).ToString().Equals(Text, ESearchCase::IgnoreCase)
				|| Enum->GetAuthoredNameStringByIndex(i).Equals(Text, ESearchCase::IgnoreCase))
			{
				OutValue = Enum->GetValueByIndex(i);
				return true;
			}
		}
		return false;
	}

	FString KeyText(const FProperty* KeyProp, const void* KeyPtr)
	{
		TSharedPtr<FJsonValue> Key = PropertyToJson(KeyProp, KeyPtr);
		FString Text;
		if (Key.IsValid() && (Key->Type == EJson::String || Key->Type == EJson::Number || Key->Type == EJson::Boolean))
		{
			if (Key->Type == EJson::Number)
			{
				return Write(Key);
			}
			if (Key->Type == EJson::Boolean)
			{
				return Key->AsBool() ? TEXT("true") : TEXT("false");
			}
			return Key->AsString();
		}
		KeyProp->ExportTextItem_Direct(Text, KeyPtr, nullptr, nullptr, PPF_None);
		return Text;
	}

	void WriteString(FString& Out, const FString& S)
	{
		Out.AppendChar(TEXT('"'));
		for (TCHAR C : S)
		{
			switch (C)
			{
			case TEXT('"'): Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			case TEXT('\b'): Out += TEXT("\\b"); break;
			case TEXT('\f'): Out += TEXT("\\f"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), (int32)C);
				}
				else
				{
					Out.AppendChar(C);
				}
			}
		}
		Out.AppendChar(TEXT('"'));
	}

	void WriteNumber(FString& Out, double V)
	{
		if (!FMath::IsFinite(V))
		{
			Out += TEXT("null");
			return;
		}
		if (V == FMath::FloorToDouble(V) && FMath::Abs(V) < 1e15)
		{
			Out += FString::Printf(TEXT("%lld"), (long long)V);
			return;
		}
		Out += FString::Printf(TEXT("%.15g"), V);
	}

	void WriteValue(FString& Out, const TSharedPtr<FJsonValue>& V);

	void WriteObject(FString& Out, const TSharedPtr<FJsonObject>& O)
	{
		Out.AppendChar(TEXT('{'));
		bool bFirst = true;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : O->Values)
		{
			if (!bFirst)
			{
				Out.AppendChar(TEXT(','));
			}
			bFirst = false;
			WriteString(Out, Pair.Key);
			Out.AppendChar(TEXT(':'));
			WriteValue(Out, Pair.Value);
		}
		Out.AppendChar(TEXT('}'));
	}

	void WriteValue(FString& Out, const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid())
		{
			Out += TEXT("null");
			return;
		}
		switch (V->Type)
		{
		case EJson::None:
		case EJson::Null:
			Out += TEXT("null");
			break;
		case EJson::String:
			WriteString(Out, V->AsString());
			break;
		case EJson::Number:
			WriteNumber(Out, V->AsNumber());
			break;
		case EJson::Boolean:
			Out += V->AsBool() ? TEXT("true") : TEXT("false");
			break;
		case EJson::Array:
		{
			Out.AppendChar(TEXT('['));
			bool bFirst = true;
			for (const TSharedPtr<FJsonValue>& E : V->AsArray())
			{
				if (!bFirst)
				{
					Out.AppendChar(TEXT(','));
				}
				bFirst = false;
				WriteValue(Out, E);
			}
			Out.AppendChar(TEXT(']'));
			break;
		}
		case EJson::Object:
			WriteObject(Out, V->AsObject());
			break;
		}
	}
}

TSharedRef<FJsonValue> Number(double Value, bool bFromFloat)
{
	if (bFromFloat && FMath::IsFinite(Value))
	{
		// Keep a float's own precision, so 0.3f reads 0.3 and not 0.300000011920929
		Value = FCString::Atod(*FString::Printf(TEXT("%.7g"), Value));
	}
	return MakeShared<FJsonValueNumber>(Value);
}

bool IsHidden(const FProperty* Property)
{
	if (!Property)
	{
		return true;
	}
	if (Property->IsA<FMulticastDelegateProperty>())
	{
		return true;
	}
	return Property->GetFName() == TEXT("UberGraphFrame");
}

FProperty* FindProperty(const UStruct* Struct, const FString& Name)
{
	if (!Struct)
	{
		return nullptr;
	}
	if (FProperty* Found = Struct->FindPropertyByName(FName(*Name)))
	{
		return Found;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (It->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase) || It->GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonValue> PropertyToJson(const FProperty* Property, const void* ValuePtr)
{
	if (!Property || !ValuePtr)
	{
		return MakeShared<FJsonValueNull>();
	}
	if (Property->IsA<FMulticastDelegateProperty>())
	{
		return nullptr;
	}
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
	{
		return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(ValuePtr));
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		const int64 Value = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
		const FString Text = EnumText(EnumProp->GetEnum(), Value);
		if (Text.IsEmpty())
		{
			return Number((double)Value);
		}
		return MakeShared<FJsonValueString>(Text);
	}
	if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
	{
		const uint8 Value = Byte->GetPropertyValue(ValuePtr);
		if (Byte->Enum)
		{
			const FString Text = EnumText(Byte->Enum, Value);
			if (!Text.IsEmpty())
			{
				return MakeShared<FJsonValueString>(Text);
			}
		}
		return Number(Value);
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		if (Numeric->IsFloatingPoint())
		{
			return Number(Numeric->GetFloatingPointPropertyValue(ValuePtr), Property->IsA<FFloatProperty>());
		}
		if (Property->IsA<FUInt64Property>())
		{
			return Number((double)Numeric->GetUnsignedIntPropertyValue(ValuePtr));
		}
		return Number((double)Numeric->GetSignedIntPropertyValue(ValuePtr));
	}
	if (const FStrProperty* Str = CastField<FStrProperty>(Property))
	{
		return MakeShared<FJsonValueString>(Str->GetPropertyValue(ValuePtr));
	}
	if (const FNameProperty* Name = CastField<FNameProperty>(Property))
	{
		return MakeShared<FJsonValueString>(Name->GetPropertyValue(ValuePtr).ToString());
	}
	if (const FTextProperty* Text = CastField<FTextProperty>(Property))
	{
		return MakeShared<FJsonValueString>(Text->GetPropertyValue(ValuePtr).ToString());
	}
	if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Property))
	{
		const FSoftObjectPtr& Ptr = *static_cast<const FSoftObjectPtr*>(ValuePtr);
		return MakeShared<FJsonValueString>(Ptr.ToSoftObjectPath().ToString());
	}
	if (const FObjectPropertyBase* Obj = CastField<FObjectPropertyBase>(Property))
	{
		const UObject* Value = Obj->GetObjectPropertyValue(ValuePtr);
		return MakeShared<FJsonValueString>(Value ? Value->GetPathName() : FString());
	}
	if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(Property))
	{
		const FScriptInterface& Value = *static_cast<const FScriptInterface*>(ValuePtr);
		return MakeShared<FJsonValueString>(Value.GetObject() ? Value.GetObject()->GetPathName() : FString());
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		return MakeShared<FJsonValueObject>(StructToJson(Struct->Struct, ValuePtr));
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Helper(Array, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Helper.Num());
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			TSharedPtr<FJsonValue> Element = PropertyToJson(Array->Inner, Helper.GetRawPtr(i));
			Out.Add(Element.IsValid() ? Element : MakeShared<FJsonValueNull>());
		}
		return MakeShared<FJsonValueArray>(Out);
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper Helper(Set, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> Out;
		for (FScriptSetHelper::FIterator It(Helper); It; ++It)
		{
			TSharedPtr<FJsonValue> Element = PropertyToJson(Set->ElementProp, Helper.GetElementPtr(PLAYTEST_CONTAINER_INDEX(It)));
			Out.Add(Element.IsValid() ? Element : MakeShared<FJsonValueNull>());
		}
		return MakeShared<FJsonValueArray>(Out);
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		FScriptMapHelper Helper(Map, ValuePtr);
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			const FString Key = KeyText(Map->KeyProp, Helper.GetKeyPtr(PLAYTEST_CONTAINER_INDEX(It)));
			TSharedPtr<FJsonValue> Value = PropertyToJson(Map->ValueProp, Helper.GetValuePtr(PLAYTEST_CONTAINER_INDEX(It)));
			Out->SetField(Key, Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		}
		return MakeShared<FJsonValueObject>(Out);
	}
	FString Exported;
	Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, nullptr, PPF_None);
	return MakeShared<FJsonValueString>(Exported);
}

TSharedPtr<FJsonValue> PropertyInContainerToJson(const FProperty* Property, const void* ContainerPtr)
{
	if (Property->ArrayDim == 1)
	{
		return PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(ContainerPtr));
	}
	TArray<TSharedPtr<FJsonValue>> Out;
	for (int32 i = 0; i < Property->ArrayDim; ++i)
	{
		TSharedPtr<FJsonValue> Element = PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(ContainerPtr, i));
		Out.Add(Element.IsValid() ? Element : MakeShared<FJsonValueNull>());
	}
	return MakeShared<FJsonValueArray>(Out);
}

TSharedRef<FJsonObject> StructToJson(const UStruct* Struct, const void* ContainerPtr)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!Struct || !ContainerPtr)
	{
		return Out;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (IsHidden(*It))
		{
			continue;
		}
		TSharedPtr<FJsonValue> Value = PropertyInContainerToJson(*It, ContainerPtr);
		if (Value.IsValid())
		{
			Out->SetField(It->GetAuthoredName(), Value);
		}
	}
	return Out;
}

UObject* ResolveObject(const FString& Path)
{
	if (Path.IsEmpty() || Path == TEXT("None"))
	{
		return nullptr;
	}
	FString Clean = Path;
	// Accept Class'/Path.Obj' style references too
	int32 Quote = INDEX_NONE;
	if (Clean.FindChar(TEXT('\''), Quote) && Clean.EndsWith(TEXT("'")))
	{
		Clean = Clean.Mid(Quote + 1, Clean.Len() - Quote - 2);
	}
	if (UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Clean))
	{
		return Found;
	}
	if (Clean.StartsWith(TEXT("/")) && !Clean.Contains(TEXT(":")))
	{
		return StaticLoadObject(UObject::StaticClass(), nullptr, *Clean, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return nullptr;
}

bool JsonToProperty(const FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Json, FString& OutError)
{
	if (!Json.IsValid())
	{
		OutError = FString::Printf(TEXT("no value for %s"), *Property->GetName());
		return false;
	}
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
	{
		bool B = false;
		if (!Json->TryGetBool(B))
		{
			double N = 0;
			FString S;
			if (Json->TryGetNumber(N))
			{
				B = N != 0;
			}
			else if (Json->TryGetString(S))
			{
				B = S.ToBool();
			}
		}
		Bool->SetPropertyValue(ValuePtr, B);
		return true;
	}
	if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		int64 Value = 0;
		if (!EnumValue(EnumProp->GetEnum(), Json, Value))
		{
			OutError = FString::Printf(TEXT("%s: unknown value for enum %s"), *Property->GetName(), *EnumProp->GetEnum()->GetName());
			return false;
		}
		EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Value);
		return true;
	}
	if (const FByteProperty* Byte = CastField<FByteProperty>(Property))
	{
		if (Byte->Enum)
		{
			int64 Value = 0;
			if (!EnumValue(Byte->Enum, Json, Value))
			{
				OutError = FString::Printf(TEXT("%s: unknown value for enum %s"), *Property->GetName(), *Byte->Enum->GetName());
				return false;
			}
			Byte->SetPropertyValue(ValuePtr, (uint8)Value);
			return true;
		}
	}
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
	{
		double N = 0;
		FString S;
		if (!Json->TryGetNumber(N))
		{
			if (Json->TryGetString(S) && S.IsNumeric())
			{
				N = FCString::Atod(*S);
			}
			else
			{
				OutError = FString::Printf(TEXT("%s: expected a number"), *Property->GetName());
				return false;
			}
		}
		if (Numeric->IsFloatingPoint())
		{
			Numeric->SetFloatingPointPropertyValue(ValuePtr, N);
		}
		else
		{
			Numeric->SetIntPropertyValue(ValuePtr, (int64)N);
		}
		return true;
	}
	if (const FStrProperty* Str = CastField<FStrProperty>(Property))
	{
		Str->SetPropertyValue(ValuePtr, Json->AsString());
		return true;
	}
	if (const FNameProperty* Name = CastField<FNameProperty>(Property))
	{
		Name->SetPropertyValue(ValuePtr, FName(*Json->AsString()));
		return true;
	}
	if (const FTextProperty* Text = CastField<FTextProperty>(Property))
	{
		Text->SetPropertyValue(ValuePtr, FText::FromString(Json->AsString()));
		return true;
	}
	if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Property))
	{
		*static_cast<FSoftObjectPtr*>(ValuePtr) = FSoftObjectPtr(FSoftObjectPath(Json->AsString()));
		return true;
	}
	if (const FObjectPropertyBase* Obj = CastField<FObjectPropertyBase>(Property))
	{
		FString Path;
		if (Json->Type == EJson::Null)
		{
			Obj->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
		}
		if (!Json->TryGetString(Path))
		{
			OutError = FString::Printf(TEXT("%s: expected an object path"), *Property->GetName());
			return false;
		}
		UObject* Value = ResolveObject(Path);
		if (!Path.IsEmpty() && Path != TEXT("None") && !Value)
		{
			OutError = FString::Printf(TEXT("Object %s does not exist"), *Path);
			return false;
		}
		if (Value)
		{
			if (const FClassProperty* ClassProp = CastField<FClassProperty>(Property))
			{
				UClass* AsClass = Cast<UClass>(Value);
				if (!AsClass || (ClassProp->MetaClass && !AsClass->IsChildOf(ClassProp->MetaClass)))
				{
					OutError = FString::Printf(TEXT("%s: %s is not a suitable class"), *Property->GetName(), *Path);
					return false;
				}
			}
			else if (Obj->PropertyClass && !Value->IsA(Obj->PropertyClass))
			{
				OutError = FString::Printf(TEXT("%s: %s is not a %s"), *Property->GetName(), *Path, *Obj->PropertyClass->GetName());
				return false;
			}
		}
		Obj->SetObjectPropertyValue(ValuePtr, Value);
		return true;
	}
	if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(Property))
	{
		UObject* Value = ResolveObject(Json->AsString());
		FScriptInterface& Target = *static_cast<FScriptInterface*>(ValuePtr);
		Target.SetObject(Value);
		Target.SetInterface(Value && Interface->InterfaceClass ? Value->GetInterfaceAddress(Interface->InterfaceClass) : nullptr);
		return true;
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (Json->TryGetObject(Object) && Object && Object->IsValid())
		{
			// A struct without a field to set (Enhanced Input's FInputActionValue keeps its value in plain C++): a value
			// given for it would go unread, and the call made with zero (an injected input that does nothing)
			if ((*Object)->Values.Num() > 0 && !TFieldIterator<FProperty>(Struct->Struct))
			{
				OutError = FString::Printf(TEXT("%s: %s has no fields to set from here%s"), *Property->GetName(),
					*Struct->Struct->GetName(), Struct->Struct->GetFName() == TEXT("InputActionValue")
						? TEXT(": press or hold the action itself (game.press / game.hold with its path), or call InjectInputVectorForAction (a plain vector)")
						: TEXT(""));
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
			{
				FProperty* Field = FindProperty(Struct->Struct, Pair.Key);
				if (!Field)
				{
					continue;  // unknown fields are ignored, as a partial struct leaves the rest as it is
				}
				if (!JsonToProperty(Field, Field->ContainerPtrToValuePtr<void>(ValuePtr), Pair.Value, OutError))
				{
					return false;
				}
			}
			return true;
		}
		FString S;
		if (Json->TryGetString(S))
		{
			if (Property->ImportText_Direct(*S, ValuePtr, nullptr, PPF_None, GLog))
			{
				return true;
			}
		}
		OutError = FString::Printf(TEXT("%s: expected an object for struct %s"), *Property->GetName(), *Struct->Struct->GetName());
		return false;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Json->TryGetArray(Items))
		{
			OutError = FString::Printf(TEXT("%s: expected a list"), *Property->GetName());
			return false;
		}
		FScriptArrayHelper Helper(Array, ValuePtr);
		Helper.Resize(Items->Num());
		for (int32 i = 0; i < Items->Num(); ++i)
		{
			if (!JsonToProperty(Array->Inner, Helper.GetRawPtr(i), (*Items)[i], OutError))
			{
				return false;
			}
		}
		return true;
	}
	if (const FSetProperty* Set = CastField<FSetProperty>(Property))
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Json->TryGetArray(Items))
		{
			OutError = FString::Printf(TEXT("%s: expected a list"), *Property->GetName());
			return false;
		}
		FScriptSetHelper Helper(Set, ValuePtr);
		Helper.EmptyElements();
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
			if (!JsonToProperty(Set->ElementProp, Helper.GetElementPtr(Index), Item, OutError))
			{
				return false;
			}
		}
		Helper.Rehash();
		return true;
	}
	if (const FMapProperty* Map = CastField<FMapProperty>(Property))
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Json->TryGetObject(Object) || !Object)
		{
			OutError = FString::Printf(TEXT("%s: expected an object"), *Property->GetName());
			return false;
		}
		FScriptMapHelper Helper(Map, ValuePtr);
		Helper.EmptyValues();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
		{
			const int32 Index = Helper.AddDefaultValue_Invalid_NeedsRehash();
			if (!JsonToProperty(Map->KeyProp, Helper.GetKeyPtr(Index), MakeShared<FJsonValueString>(Pair.Key), OutError)
				|| !JsonToProperty(Map->ValueProp, Helper.GetValuePtr(Index), Pair.Value, OutError))
			{
				return false;
			}
		}
		Helper.Rehash();
		return true;
	}
	FString S;
	if (Json->TryGetString(S) && Property->ImportText_Direct(*S, ValuePtr, nullptr, PPF_None, GLog))
	{
		return true;
	}
	OutError = FString::Printf(TEXT("%s: can't set a value of type %s"), *Property->GetName(), *Property->GetCPPType());
	return false;
}

FString Write(const TSharedPtr<FJsonValue>& Value)
{
	FString Out;
	WriteValue(Out, Value);
	return Out;
}

FString Write(const TSharedRef<FJsonObject>& Object)
{
	FString Out;
	WriteObject(Out, Object);
	return Out;
}

TSharedPtr<FJsonObject> Parse(const FString& Text)
{
	TSharedPtr<FJsonObject> Out;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Out))
	{
		return nullptr;
	}
	return Out;
}
}
