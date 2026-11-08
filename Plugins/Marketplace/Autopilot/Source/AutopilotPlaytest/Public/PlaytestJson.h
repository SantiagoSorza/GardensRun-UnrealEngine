// Reflection <-> JSON in Remote Control's value format, plus a JSON writer that prints whole numbers
// without a decimal point (as Remote Control does), so the brain sees the same values either way.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class FProperty;
class UObject;
class UStruct;

namespace PlaytestJson
{
	/** A property value as JSON: numbers, booleans, strings, references as path names ("" for none),
	 *  enums as display names, structs as objects keyed by field name, arrays and sets as lists, maps as
	 *  objects. Returns nullptr for values that aren't reported (multicast delegates). */
	AUTOPILOTPLAYTEST_API TSharedPtr<FJsonValue> PropertyToJson(const FProperty* Property, const void* ValuePtr);

	/** Every element of a property (fixed-size C arrays become lists). ContainerPtr is the object or struct. */
	AUTOPILOTPLAYTEST_API TSharedPtr<FJsonValue> PropertyInContainerToJson(const FProperty* Property, const void* ContainerPtr);

	/** Set a property value from JSON. Enums accept display names, authored names or numbers; references
	 *  accept any path StaticFindObject or LoadObject resolves. */
	AUTOPILOTPLAYTEST_API bool JsonToProperty(const FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Json, FString& OutError);

	/** All reported properties of an object or struct: authored name -> value. */
	AUTOPILOTPLAYTEST_API TSharedRef<FJsonObject> StructToJson(const UStruct* Struct, const void* ContainerPtr);

	/** A property of a struct by name (authored or internal, case-insensitive). */
	AUTOPILOTPLAYTEST_API FProperty* FindProperty(const UStruct* Struct, const FString& Name);

	/** Whether a property is left out of reads. */
	AUTOPILOTPLAYTEST_API bool IsHidden(const FProperty* Property);

	/** A number value, rounded to float precision when it came from a float. */
	AUTOPILOTPLAYTEST_API TSharedRef<FJsonValue> Number(double Value, bool bFromFloat = false);

	/** Find an object by path (StaticFindObject, then LoadObject). */
	AUTOPILOTPLAYTEST_API UObject* ResolveObject(const FString& Path);

	AUTOPILOTPLAYTEST_API FString Write(const TSharedPtr<FJsonValue>& Value);
	AUTOPILOTPLAYTEST_API FString Write(const TSharedRef<FJsonObject>& Object);
	AUTOPILOTPLAYTEST_API TSharedPtr<FJsonObject> Parse(const FString& Text);
}
