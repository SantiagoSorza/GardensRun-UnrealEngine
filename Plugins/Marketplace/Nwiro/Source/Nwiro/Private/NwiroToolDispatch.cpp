// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroToolDispatch.h"
#include "NwiroToolRegistry.h"
#include "NwiroAssetGuard.h"
#include "NwiroTransactionHelper.h"
#include "NwiroBridge.h"
#include "Misc/Base64.h"
#include "CoreGlobals.h" // GIsRunningUnattendedScript (dispatch modal-suppression guard)
#include "Kismet2/BlueprintEditorUtils.h"
#include "NwiroBlueprintTools.h"
#include "NwiroMaterialTools.h"
#include "NwiroSettingsTools.h"
#include "NwiroPCGTools.h"
#include "NwiroInputTools.h"
#include "NwiroRequestedStateTools.h"
#include "NwiroAssetTools.h"
#include "NwiroEditorTools.h"
#include "NwiroRenderTools.h"
#include "NwiroScgTools.h"
#include "NwiroDataTools.h"
#include "NwiroAnimTools.h"
#include "NwiroSequencerTools.h"
#include "NwiroAITools.h"
#include "NwiroWidgetTools.h"
#include "NwiroNiagaraTools.h"
#include "NwiroStateTreeTools.h"
#include "NwiroIKTools.h"
#include "NwiroEnvironmentTools.h"
#include "NwiroGameplayTools.h"
#include "NwiroResourceProvider.h"
#include "NwiroLevelTools.h"
#include "NwiroGASTools.h"
#include "NwiroPIETools.h"
#include "NwiroDebugTools.h"
#include "NwiroProtectedRails.h"
#include "NwiroPathSandbox.h"
#include "Json.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "GenericPlatform/GenericPlatformHttp.h"
// ElevenLabs audio import → USoundWave (factory auto-picked by extension)
#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

// ============================================================
// Tool definitions JSON — what Claude sees
// ============================================================

/** Shared builder: hardcoded built-in defs + namespaced provider defs.
 *  bForSettings=false → enabled-only provider defs (the only list a model may
 *  see). bForSettings=true → all provider defs with per-def `"disabled"` flags
 *  (settings UI only). Built-ins are identical in both modes and carry no
 *  disabled field — they are not user-disableable. */
static FString BuildToolDefinitionsJson(bool bForSettings)
{
	FString J;
	J += TEXT("[");
	J += FNwiroRequestedStateTools::DefinitionsJson();
	J += TEXT("{\"name\":\"find_blueprints\",\"description\":\"Search Blueprint assets\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"}},\"required\":[\"searchTerm\"]}},");
	J += TEXT("{\"name\":\"read_blueprint\",\"description\":\"Read Blueprint structure. Pass optional 'graph' to get full node/pin data for a specific function graph instead of the summary. Variable defaultValue reflects the existing generated-class CDO when available; declaredDefaultValue and source/status identify declaration defaults and stale or unavailable compiled state.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\",\"description\":\"Optional: name of a function or event graph to return in full detail (nodes + pins + guids). Omit for summary view.\"}},\"required\":[\"assetPath\"]}},");
	J += TEXT("{\"name\":\"create_blueprint\",\"description\":\"Create Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"parentClass\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	// >>> NWIRO_PARITY BEGIN: edit-blueprint-schema (keep in sync with the other product plugin)
	// Keep variable/default fields and the typed dispatcher contract in both products.
	J += TEXT("{\"name\":\"edit_blueprint\",\"description\":\"Edit Blueprint. ALL nodes and connections MUST be in ONE call. Node types: EnhancedInputAction(action:'/Game/Input/IA_Move.IA_Move'; only type/ref/action/x/y; outputs Started/Triggered/Completed and action-typed ActionValue),CallFunction(function:'PrintString'),Event(event:'ReceiveBeginPlay'),CustomEvent,VariableGet,VariableSet,Branch,Sequence,ForLoop,ForEachLoop,WhileLoop,Gate,DoOnce,FlipFlop,Delay,PrintString,SetTimer,DestroyActor,SetActorLocation,GetActorLocation,SpawnActor(Class pin needs TSubclassOf<Actor>: use type:'class' subtype:'ActorClassName' variable or set_pin_defaults with class/name/Blueprint path). UMG nodes are generic: CreateWidget, AddToViewport, RemoveFromParent, SetPercent, SetText; set WidgetClass/Class pins with a WidgetBlueprint asset path via set_pin_defaults. To access a component/widget/property use GetProperty NOT CallFunction: type:'GetProperty',property:'StaticMeshComponent',ownerClass:'StaticMeshActor' — Target input pin, output pin named after the property (not ReturnValue), pure node (no exec pins). To get a component from self use GetComponent: type:'GetComponent',component:'CharacterMovement' — pure. Common pins: execute/then (exec flow), ReturnValue, self/Target. VariableGet: output pin is the variable name not ReturnValue. PrintString: execute,InString,then. Event: then. Branch: execute,Condition,True,False. Delay: execute,Duration,Completed. EnhancedInputAction action must be an exact /Game object/package path already loaded in the editor; a cold or missing asset returns ACTION_NOT_LOADED and is refused before target work.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\"},\"add_nodes\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"oneOf\":[{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"enum\":[\"EnhancedInputAction\"]},\"ref\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Unique node reference for connections.\",\"pattern\":\"\\\\S\"},\"action\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact /Game InputAction object or package path of an already loaded, initialized asset. Bare names, fuzzy matches, redirects and cold asset loading are rejected before target work. Cold or missing objects return ACTION_NOT_LOADED; this does not establish asset nonexistence.\",\"pattern\":\"^/Game/[^\\\\s:]+$\"},\"x\":{\"type\":\"integer\",\"minimum\":-2147483648,\"maximum\":2147483647},\"y\":{\"type\":\"integer\",\"minimum\":-2147483648,\"maximum\":2147483647}},\"required\":[\"type\",\"ref\",\"action\"],\"additionalProperties\":false},{\"allOf\":[{\"if\":{\"properties\":{\"type\":{\"enum\":[\"VariableGet\",\"VariableSet\"]}},\"required\":[\"type\"]},\"then\":{\"required\":[\"variable\"]}}],\"properties\":{\"ref\":{\"type\":\"string\",\"description\":\"Unique ID for this node, used in connect_pins\"},\"type\":{\"type\":\"string\",\"description\":\"CallFunction, Event, CustomEvent, VariableGet, Branch, CreateWidget, AddToViewport, SetPercent, SetText, etc.\"},\"function\":{\"type\":\"string\",\"description\":\"For CallFunction: PrintString, Delay, SetText, etc.\"},\"event\":{\"type\":\"string\",\"description\":\"For Event: ReceiveBeginPlay, ReceiveTick\"},\"target\":{\"type\":\"string\",\"description\":\"For CallFunction: component/widget instance name or class name to search when resolving the function. Use for component/object/widget functions; avoids brute-force hitting the wrong class.\"},\"variable\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Required for VariableGet/VariableSet: exact member variable name, including a variable added in this call.\"}},\"not\":{\"properties\":{\"type\":{\"pattern\":\"^[Ee][Nn][Hh][Aa][Nn][Cc][Ee][Dd][Ii][Nn][Pp][Uu][Tt][Aa][Cc][Tt][Ii][Oo][Nn]$\"}},\"required\":[\"type\"]},\"type\":\"object\",\"required\":[\"ref\",\"type\"]}]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"connect_pins\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"from\":{\"type\":\"string\",\"description\":\"ref.PinName e.g. mynode.then\"},\"to\":{\"type\":\"string\",\"description\":\"ref.PinName e.g. other.execute\"}},\"required\":[\"from\",\"to\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"set_pin_defaults\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"ref\":{\"type\":\"string\"},\"pin\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"ref\",\"pin\",\"value\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"add_variables\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"type\":{\"oneOf\":[{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"Set elements must support Unreal hashing; replicated sets and all maps are rejected.\"}},\"required\":[\"base\"]}]},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected. String type only; otherwise place inside type.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"descr");
	J += TEXT("iption\":\"Set elements must support Unreal hashing; replicated sets and all maps are rejected. String type only; otherwise place inside type.\"},\"default\":{\"type\":\"string\",\"description\":\"Default import string. Legacy defaultValue/initialValue aliases accepted; supplying multiple aliases is rejected.\"},\"category\":{\"type\":\"string\"},\"tooltip\":{\"type\":\"string\"},\"replicated\":{\"type\":\"boolean\"},\"expose_on_spawn\":{\"type\":\"boolean\"},\"save_game\":{\"type\":\"boolean\"},\"instanceEditable\":{\"type\":\"boolean\",\"description\":\"Optional instance editability. Omitted means class-default-only unless expose_on_spawn:true implies true. Explicit false conflicts with expose_on_spawn:true. Underscore/case variants such as instance_editable are accepted; duplicate normalized keys reject.\"},\"instance_editable\":{\"type\":\"boolean\",\"description\":\"Optional instance editability. Omitted means class-default-only unless expose_on_spawn:true implies true. Explicit false conflicts with expose_on_spawn:true. Underscore/case variants such as instance_editable are accepted; duplicate normalized keys reject.\"},\"exposeOnSpawn\":{\"type\":\"boolean\",\"description\":\"Expose a spawn pin; true implies instance editability unless it is explicitly false. Underscore/case variants accepted; duplicate semantic spellings rejected.\"},\"defaultValue\":{\"type\":\"string\",\"description\":\"Default import string. Legacy defaultValue/initialValue aliases accepted; supplying multiple aliases is rejected.\"},\"initialValue\":{\"type\":\"string\",\"description\":\"Default import string. Legacy defaultValue/initialValue aliases accepted; supplying multiple aliases is rejected.\"},\"objectClass\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected. String type only; otherwise place inside type.\"},\"saveGame\":{\"type\":\"boolean\"}},\"required\":[\"name\",\"type\"],\"description\":\"Every supported option accepts underscore/case variants except required name/type. Duplicate semantic spellings reject in native admission even if values agree. isExposed is ambiguous and rejected. Use string type plus sibling subtype/container or a type object containing them.\",\"patternProperties\":{\"^(?!(?:instanceEditable|instance_editable)$)_*[iI]_*[nN]_*[sS]_*[tT]_*[aA]_*[nN]_*[cC]_*[eE]_*[eE]_*[dD]_*[iI]_*[tT]_*[aA]_*[bB]_*[lL]_*[eE]_*$\":{\"type\":\"boolean\",\"description\":\"Optional instance editability. Omitted means class-default-only unless expose_on_spawn:true implies true. Explicit false conflicts with expose_on_spawn:true. Underscore/case variants such as instance_editable are accepted; duplicate normalized keys reject.\"},\"^(?!(?:expose_on_spawn|exposeOnSpawn)$)_*[eE]_*[xX]_*[pP]_*[oO]_*[sS]_*[eE]_*[oO]_*[nN]_*[sS]_*[pP]_*[aA]_*[wW]_*[nN]_*$\":{\"type\":\"boolean\",\"description\":\"Expose a spawn pin; true implies instance editability unless it is explicitly false. Underscore/case variants accepted; duplicate semantic spellings rejected.\"},\"^(?!(?:save_game|saveGame)$)_*[sS]_*[aA]_*[vV]_*[eE]_*[gG]_*[aA]_*[mM]_*[eE]_*$\":{\"type\":\"boolean\"},\"^(?!(?:replicated)$)_*[rR]_*[eE]_*[pP]_*[lL]_*[iI]_*[cC]_*[aA]_*[tT]_*[eE]_*[dD]_*$\":{\"type\":\"boolean\"},\"^(?!(?:default)$)_*[dD]_*[eE]_*[fF]_*[aA]_*[uU]_*[lL]_*[tT]_*$\":{\"type\":\"string\",\"description\":\"Default import string. Legacy defaultValue/initialValue aliases accepted; supplying multiple aliases is rejected.\"},\"^(?!(?:defaultValue)$)_*[dD]_*[eE]_*[fF]_*[aA]_*[uU]_*[lL]_*[tT]_*[vV]_*[aA]_*[lL]_*[uU]_*[eE]_*$\":{\"type\":\"string\",\"description\":\"Default import string. Legacy defaultValue/initialValue aliases accepted; supplying multiple aliases is rejected.\"},\"^(?!(?:initialValue)$)_*[iI]_*[nN]_*[iI]_*[tT]_*[iI]_*[aA]_*[lL]_*[vV]_*[aA]_*[lL]_*[uU]_*[eE]_*$\":{\"type\":\"string\",\"description\":\"Default import string. Legacy defaultValue/initialValue aliases accepted; supplying multiple aliases is rejected.\"},\"^(?!(?:category)$)_*[cC]_*[aA]_*[tT]_*[eE]_*[gG]_*[oO]_*[rR]_*[yY]_*$\":{\"type\":\"string\"},\"^(?!(?:tooltip)$)_*[tT]_*[oO]_*[oO]_*[lL]_*[tT]_*[iI]_*[pP]_*$\":{\"type\":\"string\"},\"^(?!(?:subtype)$)_*[sS]_*[uU]_*[bB]_*[tT]_*[yY]_*[pP]_*[eE]_*$\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected. String type only; otherwise place inside type.\"},\"^(?!(?:objectClass)$)_*[oO]_*[bB]_*[jJ]_*[eE]_*[cC]_*[tT]_*[cC]_*[lL]_*[aA]_*[sS]_*[sS]_*$\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected. String type only; otherwise place inside type.\"},\"^(?!(?:container)$)_*[cC]_*[oO]_*[nN]_*[tT]_*[aA]_*[iI]_*[nN]_*[eE]_*[rR]_*$\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"Set elements must support Unreal hashing; replicated sets and all maps are rejected. String type only; otherwise place inside type.\"}},\"additionalProperties\":false,\"propertyNames\":{\"not\":{\"pattern\":\"^_*[iI]_*[sS]_*[eE]_*[xX]_*[pP]_*[oO]_*[sS]_*[eE]_*[dD]_*$\"}},\"not\":{\"allOf\":[{\"not\":{\"patternProperties\":{\"^_*[iI]_*[nN]_*[sS]_*[tT]_*[aA]_*[nN]_*[cC]_*[eE]_*[eE]_*[dD]_*[iI]_*[tT]_*[aA]_*[bB]_*[lL]_*[eE]_*$\":{\"not\":{\"const\":false}}}}},{\"not\":{\"patternProperties\":{\"^_*[eE]_*[xX]_*[pP]_*[oO]_*[sS]_*[eE]_*[oO]_*[nN]_*[sS]_*[pP]_*[aA]_*[wW]_*[nN]_*$\":{\"not\":{\"const\":true}}}}}]}}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}],\"description\":\"Validates every variable shape, type and option before target load/create. Rejected batches on existing targets preserve state and skip compile/save. Implicit target creation and unrelated operations are outside this guarantee; create first, then edit. Use a separate call for reparenting.\"},\"remove_variables\":{");
	J += TEXT("\"oneOf\":[{\"type\":\"array\",\"items\":{\"description\":\"Variable name (string) or object with a 'name' field\"}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"add_components\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"description\":\"Each item needs 'name' and either 'class' or 'type'. Example: {'name':'CubeMesh','class':'StaticMeshComponent','properties':{'static_mesh':'/Engine/BasicShapes/Cube.Cube'}}\",\"properties\":{\"name\":{\"type\":\"string\"},\"class\":{\"type\":\"string\",\"description\":\"Component class, e.g. StaticMeshComponent ('type' also accepted)\"},\"parent\":{\"type\":\"string\"},\"makeRoot\":{\"type\":\"boolean\"},\"properties\":{\"type\":\"object\",\"description\":\"Initial property values, e.g. {'static_mesh':'/Engine/BasicShapes/Cube.Cube'}\"}},\"required\":[\"name\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"add_event_dispatchers\":{\"oneOf\":[{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"params\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"type\":{\"oneOf\":[{\"type\":\"string\",\"minLength\":1},{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1},\"subtype\":{\"type\":\"string\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"]}},\"required\":[\"base\"]}]}},\"required\":[\"name\",\"type\"]}}},\"required\":[\"name\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}]},\"compile\":{\"type\":\"boolean\"},\"add_functions\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"pure\":{\"type\":\"boolean\"},\"override\":{\"type\":\"boolean\",\"description\":\"Use an inherited signature for a new graph; supplied parameters must match it. Existing-graph override requests are no-ops and ignore syntactically valid supplied signature/purity options. Compile required dependencies separately first.\"},\"inputs\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"type\":{\"oneOf\":[{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"Set elements must support Unreal hashing; maps are rejected.\"}},\"required\":[\"base\"]}]},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"String type only; exact loaded subtype. Otherwise put subtype inside the type object.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"String type only; otherwise place inside the type object.\"},\"objectClass\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"object_class\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"default\":{\"type\":\"string\",\"description\":\"Optional input literal default. Containers/reference inputs require wired values. Object/class defaults use exact already-loaded paths or None.\"}},\"required\":[\"name\",\"type\"]}},\"outputs\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"type\":{\"oneOf\":[{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"Set elements must support Unreal hashing; maps are rejected.\"}},\"required\":[\"base\"]}]},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"String type only; exact loaded subtype. Otherwise put subtype inside the type object.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"String type only; otherwise place inside the type object.\"},\"objectClass\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"object_class\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"}},\"required\":[\"name\",\"type\"]}},\"redefine\":{\"type\":\"boolean\",\"description\":\"Explicit");
	J += TEXT("ly replace an existing editable ordinary function signature. Omitted inputs, outputs and pure retain their current declarations. Linked parameters must retain name/type; inherited/interface functions cannot be redefined.\"}},\"required\":[\"name\"]}},{\"type\":\"string\",\"description\":\"JSON-encoded array (compatibility; prefer a real array)\"}],\"description\":\"Prepare and validate the complete function batch before target creation or edits. Accept a real array or legacy JSON-encoded array. Existing-target preflight rejection does not mutate, compile, or save; implicit target creation and unrelated later failures are outside batch atomicity. Reparenting, interface attachment/removal, and variable renaming require separate calls. Function-only no-ops never save and suppress implicit compilation; explicit compile:true retains the existing compile gate. Compile matching override dependencies separately first.\"}},\"required\":[\"blueprint\"]}},");
	// <<< NWIRO_PARITY END: edit-blueprint-schema
	J += TEXT("{\"name\":\"find_blueprint_nodes\",\"description\":\"Search node types Also returns nodeRecipes for Enhanced Input event-node searches; these are add_nodes recipes, not UFunctions.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}},");
	J += TEXT("{\"name\":\"render_blueprint_graph\",\"description\":\"Render a Blueprint graph fitted to the requested canvas. Dimensions are clamped to 64..4096. organize:true retains its existing layout/save behavior; use organize:false to inspect without reorganizing.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\",\"description\":\"Graph name (default: EventGraph or first ubergraph)\"},\"organize\":{\"type\":\"boolean\",\"description\":\"Auto-arrange nodes before rendering (default true)\"},\"width\":{\"type\":\"number\"},\"height\":{\"type\":\"number\"},\"saveTo\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"organize_blueprint_nodes\",\"description\":\"Auto-arrange a Blueprint graph's nodes so they don't overlap, laid out left-to-right along exec flow (headless layered layout). Organizes the named graph, or all graphs if none given. Saves the asset.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\",\"description\":\"Graph name (default: all graphs)\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"set_cdo_property\",\"description\":\"Set CDO property\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"property\",\"value\"]}},");
	J += TEXT("{\"name\":\"get_world_settings\",\"description\":\"Get world settings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"set_world_settings\",\"description\":\"Set gravity/killZ\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"globalGravityZ\":{\"type\":\"number\"},\"killZ\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"set_game_mode\",\"description\":\"Set game mode, pawn, controller, HUD, game state, spectator classes\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"gameModeClass\":{\"type\":\"string\"},\"defaultPawnClass\":{\"type\":\"string\"},\"playerControllerClass\":{\"type\":\"string\"},\"hudClass\":{\"type\":\"string\"},\"gameStateClass\":{\"type\":\"string\"},\"spectatorClass\":{\"type\":\"string\"}},\"required\":[\"gameModeClass\"]}},");
	J += TEXT("{\"name\":\"get_project_settings\",\"description\":\"Read project settings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"category\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"set_project_settings\",\"description\":\"Set project settings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"settings\":{\"type\":\"object\"}},\"required\":[\"settings\"]}},");
	J += TEXT("{\"name\":\"find_materials\",\"description\":\"Search materials\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"}},\"required\":[\"searchTerm\"]}},");
	J += TEXT("{\"name\":\"inspect_material\",\"description\":\"Read material params\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"}},\"required\":[\"assetPath\"]}},");
	J += TEXT("{\"name\":\"create_material\",\"description\":\"Create empty material asset. Use edit_material after this to add expressions and connections.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"edit_material\",\"description\":\"Edit material graph. Send ALL expressions AND connections in ONE call — refs only exist within that call. expressions: [{type,name}] where type is UE class (TextureSampleParameter2D,ScalarParameter,VectorParameter,Multiply,Divide,Add,Subtract,Lerp,Ceil,Floor,Clamp,Power,Desaturation,OneMinus,ComponentMask,VertexColor,WorldPosition,Time,Sine,Cosine,Abs,Dot,Normalize,Fresnel,Constant,Constant3Vector). name sets both ref AND UE5 ParameterName. connections: [{from,to}] where from/to is RefName (default output) or RefName.Pin. Material pins: Material.BaseColor,Material.Normal,Material.Roughness,Material.Metallic,Material.EmissiveColor,Material.Opacity,Material.AmbientOcclusion. Math pins: A,B. Lerp: A,B,Alpha. Desaturation: Input(0),Fraction. MaterialFunctionCall expressions bind via 'materialFunction' (path of the function to call); aliases accepted: function, functionPath, function_path, material_function, assetPath (alias here is scoped to the expression object — distinct from the top-level assetPath parameter).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"},\"expressions\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\"}},{\"type\":\"string\"}]},\"connections\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\"}},{\"type\":\"string\"}]}},\"required\":[\"assetPath\"]}},");
	J += TEXT("{\"name\":\"inspect_material_graph\",\"description\":\"Inspect material graph structure and expressions\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"material\":{\"type\":\"string\"}},\"required\":[]}},");
	J += TEXT("{\"name\":\"create_material_instance\",\"description\":\"Create a material instance from a parent material, or update the instance of that name: texture, scalar, vector and static switch parameters by parameter name. Unknown switch names and missing textures are skipped and listed in warnings. save: true writes the asset to disk and reports saved; by default the asset is only marked dirty.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Instance name; MI_ is prepended when missing\"},\"parent\":{\"type\":\"string\",\"description\":\"Parent material path\"},\"folder\":{\"type\":\"string\",\"description\":\"Destination folder, default /Game/Materials\"},\"scalars\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"value\":{\"type\":\"number\"}},\"required\":[\"name\",\"value\"]}},\"vectors\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"r\":{\"type\":\"number\"},\"g\":{\"type\":\"number\"},\"b\":{\"type\":\"number\"},\"a\":{\"type\":\"number\"}},\"required\":[\"name\",\"r\",\"g\",\"b\"]}},\"textures\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\",\"path\"]}},\"switches\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"value\":{\"type\":\"boolean\"}},\"required\":[\"name\",\"value\"]}},\"clearTextures\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Texture parameters whose instance override is removed, so the parent texture shows again\"},\"save\":{\"type\":\"boolean\"},\"resetParent\":{\"type\":\"boolean\",\"description\":\"Re-parent an existing instance to parent when it differs; default false keeps its parent\"},\"reveal\":{\"type\":\"boolean\",\"description\":\"Select the instance in the Content Browser afterwards\"},\"parentCopy\":{\"type\":\"string\",\"description\":\"A /Game asset path; the parent becomes a project copy there, made once and never overwritten\"}},\"required\":[\"name\",\"parent\"]}},");
	J += TEXT("{\"name\":\"apply_material\",\"description\":\"Apply material to actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"materialPath\":{\"type\":\"string\"},\"actorPath\":{\"type\":\"string\"}},\"required\":[\"materialPath\",\"actorPath\"]}},");
	J += TEXT("{\"name\":\"find_textures\",\"description\":\"Search textures\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"}},\"required\":[\"searchTerm\"]}},");
	J += TEXT("{\"name\":\"create_input_action\",\"description\":\"Create input action\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"valueType\":{\"type\":\"string\"}},\"required\":[\"name\",\"valueType\"]}},");
	J += TEXT("{\"name\":\"create_input_mapping_context\",\"description\":\"Create input mapping\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"mappings\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"name\",\"mappings\"]}},");
	J += TEXT("{\"name\":\"find_input_actions\",\"description\":\"Search input actions\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"get_level_actors\",\"description\":\"List level actors\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"classFilter\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"find_static_meshes\",\"description\":\"Search static meshes\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"clear_graph\",\"description\":\"DESTRUCTIVE: removes ALL nodes from a blueprint graph. Only use this when the user explicitly asks to start the graph over. NEVER call this in response to an edit_blueprint or connect_pins error — those errors include the available node refs and pin names so you can self-correct without wiping anything.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\"},\"confirm\":{\"type\":\"string\",\"const\":\"user_requested\",\"description\":\"Only provide when the user explicitly requested clearing this graph.\"}},\"required\":[\"blueprint\",\"confirm\"]}},");
	J += TEXT("{\"name\":\"delete_blueprint\",\"description\":\"Delete a blueprint asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"}},\"required\":[\"assetPath\"]}},");
	J += TEXT("{\"name\":\"duplicate_blueprint\",\"description\":\"Duplicate a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"},\"destinationPath\":{\"type\":\"string\"}},\"required\":[\"source\",\"newName\"]}},");
	J += TEXT("{\"name\":\"rename_blueprint\",\"description\":\"Rename a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"}},\"required\":[\"assetPath\",\"newName\"]}},");
	J += TEXT("{\"name\":\"spawn_actor\",\"description\":\"Spawn an actor in the level. Provide exactly one of: class (an Actor class name, e.g. StaticMeshActor), blueprint (a /Game/... Blueprint path), or staticMesh (a /Game/... StaticMesh path, which spawns a StaticMeshActor with that mesh already assigned). An unresolvable value is an error, not an empty actor.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"class\":{\"type\":\"string\"},\"blueprint\":{\"type\":\"string\"},\"staticMesh\":{\"type\":\"string\",\"description\":\"Full static mesh asset path, e.g. /Game/Meshes/SM_Rock_01\"},\"label\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"delete_actor\",\"description\":\"Delete an actor from the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"transform_actor\",\"description\":\"Move/rotate/scale an actor. Location: x,y,z. Rotation: pitch,yaw,roll. Scale: scale (uniform) or scaleX,scaleY,scaleZ\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},\"yaw\":{\"type\":\"number\"},\"roll\":{\"type\":\"number\"},\"scale\":{\"type\":\"number\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"get_actor_property\",\"description\":\"Get a property value from an actor in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"}},\"required\":[\"name\",\"property\"]}},");
	J += TEXT("{\"name\":\"set_actor_property\",\"description\":\"Set a property value on an actor in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"name\",\"property\",\"value\"]}},");
	J += TEXT("{\"name\":\"execute_python\",\"description\":\"Execute Python in the live editor by default. Explicit execution_mode=isolated starts a cancellable Windows editor commandlet job using saved project state. Poll/cancel with action and job_id. Timeout includes startup. Process termination does not roll back prior file or asset writes; unsaved live state is unavailable. Retains the latest 32 job results for this editor session; at most 2 concurrent jobs.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"execution_mode\":{\"type\":\"string\",\"enum\":[\"live\",\"isolated\"],\"default\":\"live\"},\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"status\",\"cancel\"],\"default\":\"start\"},\"code\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":1048576},\"job_id\":{\"type\":\"string\",\"minLength\":1},\"timeout_seconds\":{\"type\":\"number\",\"minimum\":1,\"maximum\":3600,\"default\":120}},\"if\":{\"required\":[\"execution_mode\",\"action\"],\"properties\":{\"execution_mode\":{\"const\":\"isolated\"},\"action\":{\"enum\":[\"status\",\"cancel\"]}}},\"then\":{\"required\":[\"job_id\"]},\"else\":{\"required\":[\"code\"]},\"allOf\":[{\"if\":{\"required\":[\"execution_mode\"],\"properties\":{\"execution_mode\":{\"const\":\"isolated\"}}},\"else\":{\"not\":{\"anyOf\":[{\"required\":[\"action\"]},{\"required\":[\"job_id\"]},{\"required\":[\"timeout_seconds\"]}]}}},{\"if\":{\"required\":[\"execution_mode\",\"action\"],\"properties\":{\"execution_mode\":{\"const\":\"isolated\"},\"action\":{\"enum\":[\"status\",\"cancel\"]}}},\"then\":{\"not\":{\"anyOf\":[{\"required\":[\"code\"]},{\"required\":[\"timeout_seconds\"]}]}},\"else\":{\"not\":{\"required\":[\"job_id\"]}}}]}},");
	J += TEXT("{\"name\":\"search_assets\",\"description\":\"Search all project assets by name. Returns name, path, and class.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}},");
	J += TEXT("{\"name\":\"write_file\",\"description\":\"Write content to a file. Creates or overwrites. Requires 'File Editor' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute or project-relative file path\"},\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},\"required\":[\"file_path\",\"content\"]}},");
	J += TEXT("{\"name\":\"read_file\",\"description\":\"Read content from a file. Requires 'File Editor' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\",\"description\":\"Absolute or project-relative file path\"}},\"required\":[\"file_path\"]}},");
	J += TEXT("{\"name\":\"delete_file\",\"description\":\"Delete a file from disk. Requires 'File Editor' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"}},\"required\":[\"file_path\"]}},");
	J += TEXT("{\"name\":\"rename_file\",\"description\":\"Rename or move a file. Requires 'File Editor' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"old_path\":{\"type\":\"string\"},\"new_path\":{\"type\":\"string\"}},\"required\":[\"old_path\",\"new_path\"]}},");

	// Blueprint standalone tools
	J += TEXT("{\"name\":\"delete_node\",\"description\":\"Delete node(s) from a blueprint graph by ref\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"graph\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"refs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"create_function_graph\",\"description\":\"Create validated function signatures on an existing Blueprint. Use single name/inputs/outputs/pure/override or functions batch; a present functions array takes precedence over those single-function fields. Empty batch is a no-op. Function and parameter names preserve engine-valid spaces. Types retain exact loaded subtype and none/array/hashable-set containers; maps are rejected. Preflight rejects before target mutation. Successful new standalone functions compile without saving; inspect actual compile status. Entry node reference equals the function name. Input parameters accept string default literals. Use redefine:true for explicit ordinary signature replacement; omitted sides are retained and incompatible linked-pin changes are rejected before mutation.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\",\"minLength\":1},\"pure\":{\"type\":\"boolean\"},\"override\":{\"type\":\"boolean\",\"description\":\"Use an inherited signature for a new graph; supplied parameters must match it. Existing-graph override requests are no-ops and ignore syntactically valid supplied signature/purity options. Compile required dependencies separately first.\"},\"inputs\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"type\":{\"oneOf\":[{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"Set elements must support Unreal hashing; maps are rejected.\"}},\"required\":[\"base\"]}]},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"String type only; exact loaded subtype. Otherwise put subtype inside the type object.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"String type only; otherwise place inside the type object.\"},\"objectClass\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"object_class\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"default\":{\"type\":\"string\",\"description\":\"Optional input literal default. Containers/reference inputs require wired values. Object/class defaults use exact already-loaded paths or None.\"}},\"required\":[\"name\",\"type\"]}},\"outputs\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"type\":{\"oneOf\":[{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"Set elements must support Unreal hashing; maps are rejected.\"}},\"required\":[\"base\"]}]},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"String type only; exact loaded subtype. Otherwise put subtype inside the type object.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"String type only; otherwise place inside the type object.\"},\"objectClass\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"object_class\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"}},\"required\":[\"name\",\"type\"]}},\"functions\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"pure\":{\"type\":\"boolean\"},\"override\":{\"type\":\"boolean\",\"description\":\"Use an inherited signature for a new graph; supplied parameters must match it. Existing-graph override requests are no-ops and ignore syntactically valid supplied signature/purity options. Compile required dependencies separately first.\"},\"inputs\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"type\":{\"oneOf\":[{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},{\"type\":\"");
	J += TEXT("object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"Set elements must support Unreal hashing; maps are rejected.\"}},\"required\":[\"base\"]}]},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"String type only; exact loaded subtype. Otherwise put subtype inside the type object.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"String type only; otherwise place inside the type object.\"},\"objectClass\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"object_class\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"default\":{\"type\":\"string\",\"description\":\"Optional input literal default. Containers/reference inputs require wired values. Object/class defaults use exact already-loaded paths or None.\"}},\"required\":[\"name\",\"type\"]}},\"outputs\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1},\"type\":{\"oneOf\":[{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1,\"description\":\"bool/boolean, byte, int/integer/int32, int64, float, double, string/fstring, name/fname, text/ftext, vector/fvector, rotator/frotator, transform/ftransform, linearcolor/color; object/class/struct/enum require subtype. Loaded raw type names also accepted.\"},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact loaded Blueprint-eligible class/struct/enum name or object path. Required for explicit object/class/struct/enum. Ambiguous, unloaded, wrong-kind and transient references are rejected.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"Set elements must support Unreal hashing; maps are rejected.\"}},\"required\":[\"base\"]}]},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"String type only; exact loaded subtype. Otherwise put subtype inside the type object.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"],\"description\":\"String type only; otherwise place inside the type object.\"},\"objectClass\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"},\"object_class\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Compatibility subtype alias. Prefer subtype; conflicting aliases/forms are rejected.\"}},\"required\":[\"name\",\"type\"]}},\"redefine\":{\"type\":\"boolean\",\"description\":\"Explicitly replace an existing editable ordinary function signature. Omitted inputs, outputs and pure retain their current declarations. Linked parameters must retain name/type; inherited/interface functions cannot be redefined.\"}},\"required\":[\"name\"]}},\"redefine\":{\"type\":\"boolean\",\"description\":\"Explicitly replace an existing editable ordinary function signature. Omitted inputs, outputs and pure retain their current declarations. Linked parameters must retain name/type; inherited/interface functions cannot be redefined.\"}},\"required\":[\"blueprint\"],\"anyOf\":[{\"required\":[\"name\"]},{\"required\":[\"functions\"]}]}},");
	J += TEXT("{\"name\":\"add_interface\",\"description\":\"Add a Blueprint Interface to a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"interface\":{\"type\":\"string\"},\"interfaces\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"remove_interface\",\"description\":\"Remove a Blueprint Interface from a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"interface\":{\"type\":\"string\"},\"interfaces\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"create_event_dispatcher\",\"description\":\"Create one or more Blueprint Event Dispatchers with validated typed signature graphs. Type is a primitive/raw resolvable type string or {base,subtype?,container?:none|array|set}; maps are rejected.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"params\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"type\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"object\",\"properties\":{\"base\":{\"type\":\"string\"},\"subtype\":{\"type\":\"string\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"]}},\"required\":[\"base\"]}]}},\"required\":[\"name\",\"type\"]}},\"dispatchers\":{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"params\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"type\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"object\",\"properties\":{\"base\":{\"type\":\"string\"},\"subtype\":{\"type\":\"string\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\",\"set\"]}},\"required\":[\"base\"]}]}},\"required\":[\"name\",\"type\"]}}},\"required\":[\"name\"]}}},\"required\":[\"blueprint\"],\"anyOf\":[{\"required\":[\"name\"]},{\"required\":[\"dispatchers\"]}]}},");
	J += TEXT("{\"name\":\"reparent_blueprint\",\"description\":\"Change a blueprint parent class\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"parentClass\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"parentClass\"]}},");
	J += TEXT("{\"name\":\"remove_component\",\"description\":\"Remove component(s) from a blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"components\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"edit_component\",\"description\":\"Set properties on an EXISTING blueprint component (mesh, physics, etc). Cannot add new components — use edit_blueprint with add_components for that.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"properties\":{\"type\":\"object\"},\"components\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"blueprint\"]}},");

	// Material standalone tools
	J += TEXT("{\"name\":\"delete_material\",\"description\":\"Delete a material asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"set_material_property\",\"description\":\"Set material properties: blend_mode (Opaque/Masked/Translucent/Additive/Modulate), shading_model (DefaultLit/Unlit/Subsurface/ClearCoat/TwoSidedFoliage), two_sided (bool), opacity_mask_clip_value (number)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"material\":{\"type\":\"string\"},\"blend_mode\":{\"type\":\"string\"},\"shading_model\":{\"type\":\"string\"},\"two_sided\":{\"type\":\"boolean\"},\"opacity_mask_clip_value\":{\"type\":\"number\"}},\"required\":[\"material\"]}},");
	J += TEXT("{\"name\":\"edit_material_instance\",\"description\":\"Edit material instance parameters\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"scalars\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"vectors\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"textures\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"delete_expression\",\"description\":\"Delete a validated batch of target-owned material expressions by ref/name/parameter name or GUID. Missing, ambiguous or foreign references reject the entire batch before deletion.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"material\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"refs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"material\"]}},");

	// Material Function tools (Policy 1.1 strict)
	J += TEXT("{\"name\":\"edit_material_function\",");
	J += TEXT("\"description\":\"Edit a UMaterialFunction asset (Policy 1.1 strict). Target: assetPath. Nested contract: addInputs [{name or ref,type,sortPriority,description,x,y,previewValue,useAsDefault,defaultValue}], type enum Scalar|Vector2|Vector3|Vector4|Texture2D|TextureCube|StaticBool|MaterialAttributes. previewValue is the canonical default-value slot; its shape depends on type — Scalar:number, Vector2/3/4:array[N] of numbers, StaticBool:boolean. Texture2D/TextureCube/MaterialAttributes do not accept previewValue (INVALID_VALUE). useAsDefault:boolean controls whether PreviewValue substitutes when the input is unconnected at the call site. defaultValue is an alias accepted for AI compatibility; it normalizes to previewValue plus useAsDefault:true (unless useAsDefault is supplied explicitly) and emits an ALIAS_NORMALIZED warning naming the canonical pair. addOutputs [{name or ref,sortPriority,description,x,y}]. addExpressions [{name or ref,type,class,x,y,materialFunction,properties,default,defaultValue,texture,value,r,g,b,a,exponent,exponentIn}]. For MaterialFunctionCall, use canonical materialFunction only; aliases such as function/functionPath/material_function/assetPath are rejected. connect [{from,to}] uses dot pin syntax, e.g. {\\\"from\\\":\\\"Alpha\\\",\\\"to\\\":\\\"LerpNode.Alpha\\\"}. Colon syntax (Ref:Pin) is accepted as an alias and normalized to dot syntax with an ALIAS_NORMALIZED warning naming the canonical form. disconnect [{to}] only, same dot syntax. removeInputs/removeOutputs/deleteExpressions are string arrays. setMetadata {description,category,exposeToLibrary}. clearGraph true removes graph expressions. Array fields (addInputs, addOutputs, addExpressions, connect, disconnect, removeInputs, removeOutputs, deleteExpressions) accept real JSON arrays (canonical) or, as a framework-compatibility input only, a JSON-encoded string representation; the string form is parsed in place and emits an ALIAS_NORMALIZED warning naming `array` as canonical. Unknown or invalid top-level/nested schema fails before mutation. Response envelope always includes success, policy_version, _callId, messages, created, modified, skipped, warnings, errors, and failure error string.\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"assetPath\":{\"type\":\"string\"},\"clearGraph\":{\"type\":\"boolean\"},\"addInputs\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"type\":{\"type\":\"string\",\"enum\":[\"Scalar\",\"Vector2\",\"Vector3\",\"Vector4\",\"Texture2D\",\"TextureCube\",\"StaticBool\",\"MaterialAttributes\"]},\"sortPriority\":{\"type\":\"number\"},\"description\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"previewValue\":{},\"useAsDefault\":{\"type\":\"boolean\"},\"defaultValue\":{}},\"required\":[\"type\"],\"anyOf\":[{\"required\":[\"name\"]},{\"required\":[\"ref\"]}]},\"description\":\"Canonical JSON array. JSON-encoded string accepted as compatibility input.\"},{\"type\":\"string\"}]},\"addOutputs\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"sortPriority\":{\"type\":\"number\"},\"description\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}},\"anyOf\":[{\"required\":[\"name\"]},{\"required\":[\"ref\"]}]}},{\"type\":\"string\"}]},\"addExpressions\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"name\":{\"type\":\"string\"},\"ref\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},\"class\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"materialFunction\":{\"type\":\"string\"},\"properties\":{\"type\":\"object\"},\"default\":{},\"defaultValue\":{},\"texture\":{\"type\":\"string\"},\"value\":{},\"r\":{\"type\":\"number\"},\"g\":{\"type\":\"number\"},\"b\":{\"type\":\"number\"},\"a\":{\"type\":\"number\"},\"exponent\":{\"type\":\"number\"},\"exponentIn\":{\"type\":\"number\"}},\"required\":[\"type\"],\"anyOf\":[{\"required\":[\"name\"]},{\"required\":[\"ref\"]}]}},{\"type\":\"string\"}]},\"connect\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"from\",\"to\"]}},{\"type\":\"string\"}]},\"disconnect\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"to\":{\"type\":\"string\"}},\"required\":[\"to\"]}},{\"type\":\"string\"}]},\"removeInputs\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"string\"}},{\"type\":\"string\"}]},\"removeOutputs\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"string\"}},{\"type\":\"string\"}]},\"deleteExpressions\":{\"oneOf\":[{\"type\":\"array\",\"items\":{\"type\":\"string\"}},{\"type\":\"string\"}]},\"setMetadata\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"description\":{\"type\":\"string\"},\"category\":{\"type\":\"string\"},\"exposeToLibrary\":{\"type\":\"boolean\"}}}},\"required\":[\"assetPath\"]}},");

	J += TEXT("{\"name\":\"inspect_material_function\",");
	J += TEXT("\"description\":\"Read a UMaterialFunction asset (Policy 1.1 strict, read-only). Target: assetPath (canonical; 'path' is rejected). Returns the function's name, description, category, exposedToLibrary, inputs[], outputs[], expressions[], connections[], referencedBy[] plus the policy envelope (success, policy_version, _callId, messages, created, modified, skipped, warnings, errors). Orphan expressions (no outgoing connection) are reported as structured warnings[] with code ORPHAN_EXPRESSION (tool-specific extension). Function inputs whose UE input type is not representable in edit_material_function (e.g. Texture2DArray, VolumeTexture) are still emitted in inputs[] with type:'Unsupported' plus a warnings[] entry with code UNSUPPORTED_INPUT_TYPE; allowed[] lists the eight contract types. Failure cases emit NOT_FOUND when the asset doesn't exist and MISSING_REQUIRED_FIELD when assetPath is omitted.\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"assetPath\":{\"type\":\"string\"}},\"required\":[\"assetPath\"]}},");

	J += TEXT("{\"name\":\"find_material_functions\",");
	J += TEXT("\"description\":\"Search for UMaterialFunction assets (Policy 1.1 strict, read-only). Inputs: searchTerm (case-insensitive substring; omit to list all), searchRoot (package path to search under, default /Game), maxResults (default 50, capped at 200). Returns functions[] where each row uses canonical assetPath plus name, description, exposedToLibrary, category. Envelope: success, policy_version, _callId, count, truncated, and the six standard arrays. When truncated, also emits returnedCount, totalAllowed, and discoveryTool (set to this tool name so the AI knows to call again with a narrower searchTerm or higher maxResults).\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"searchTerm\":{\"type\":\"string\"},\"searchRoot\":{\"type\":\"string\"},\"maxResults\":{\"type\":\"number\"}}}},");

	J += TEXT("{\"name\":\"create_material_function\",");
	J += TEXT("\"description\":\"Create a new empty UMaterialFunction asset (Policy 1.1 strict). Canonical happy path: send 'assetPath' as a full destination path like '/Game/Foo/MF_Bar'. The 'name + path' pair is accepted only as a migration alias and emits ALIAS_NORMALIZED warnings. If both 'assetPath' and 'name'/'path' are supplied and they resolve to different targets, the call fails with INVALID_VALUE before any mutation. 'MF_' prefix is auto-applied to missing names (warned). Optional metadata: description, category, exposeToLibrary (default true). Fails with ASSET_ALREADY_EXISTS when the target already exists, MISSING_REQUIRED_FIELD when neither assetPath nor name is supplied, INVALID_VALUE for malformed paths or destinations outside /Game/. Returns the policy envelope with one created[] entry: {op:'create_material_function', userName, assignedRef:<canonical assetPath>, type:'MaterialFunction'}.\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"assetPath\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"category\":{\"type\":\"string\"},\"exposeToLibrary\":{\"type\":\"boolean\"}}}},");

	J += TEXT("{\"name\":\"delete_material_function\",");
	J += TEXT("\"destructive\":true,");
	J += TEXT("\"description\":\"Delete a UMaterialFunction asset (Policy 1.1 strict M2-destructive). Two-flag safety: 'preview' (default true) controls preview vs execute; 'allowBrokenReferences' (default false) is additionally required to delete an asset that has referencers. Send 'preview:false' alone for unreferenced assets; for referenced assets you MUST send BOTH 'preview:false' AND 'allowBrokenReferences:true'. PREVIEW response: success:true, preview:true, destructive:true, plus wouldDelete[] (canonical M2) and wouldModify[] with one {op:'break_reference', assetPath, type} row per referencer; no mutation occurs. EXECUTE success: deletion is recorded in modified[] as {op:'delete', assetPath, type:'MaterialFunction'} alongside any break_reference rows for now-broken referencers. There is no deleted[] array — deletions live in modified[] per rule 16. REFUSAL on preview:false without allowBrokenReferences:true returns success:false with errors[].code='WOULD_BREAK_REFERENCES' (allowed:['true']). Tool-specific extension codes emitted: WOULD_BREAK_REFERENCES (errors[], when allowBrokenReferences is required but missing), BROKEN_REFERENCES (warnings[], after a successful destructive delete with referencers). Both boolean fields (preview, allowBrokenReferences) accept real JSON booleans (canonical) or, as a framework-compatibility input only, the exact JSON strings 'true' / 'false'; the string form is normalized in place and emits an ALIAS_NORMALIZED warning naming `boolean` as canonical. Fails with NOT_FOUND when the asset doesn't exist and INVALID_VALUE when the target asset is not a MaterialFunction. preview/refusal branches never mutate; allowBrokenReferences is checked before DeleteAsset is called.\",");
	J += TEXT("\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"_callId\":{\"type\":\"string\"},\"assetPath\":{\"type\":\"string\"},\"preview\":{\"oneOf\":[{\"type\":\"boolean\"},{\"type\":\"string\",\"enum\":[\"true\",\"false\"]}]},\"allowBrokenReferences\":{\"oneOf\":[{\"type\":\"boolean\"},{\"type\":\"string\",\"enum\":[\"true\",\"false\"]}]}},\"required\":[\"assetPath\"]}},");

	// Actor tools
	J += TEXT("{\"name\":\"duplicate_actor\",\"description\":\"Duplicate an actor in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"},\"offsetX\":{\"type\":\"number\"},\"offsetY\":{\"type\":\"number\"},\"offsetZ\":{\"type\":\"number\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"rename_actor\",\"description\":\"Rename an actor label in the level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"newName\":{\"type\":\"string\"}},\"required\":[\"name\",\"newName\"]}},");
	J += TEXT("{\"name\":\"attach_actor\",\"description\":\"Attach a child actor to a parent actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"child\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"},\"socketName\":{\"type\":\"string\"},\"rule\":{\"type\":\"string\",\"description\":\"KeepWorld (default), KeepRelative, SnapToTarget\"}},\"required\":[\"child\",\"parent\"]}},");
	J += TEXT("{\"name\":\"detach_actor\",\"description\":\"Detach an actor from its parent\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"rule\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"select_actor\",\"description\":\"Select actor(s) in the editor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"names\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}},");

	// Input tools
	J += TEXT("{\"name\":\"delete_input_action\",\"description\":\"Delete one exact InputAction or InputMappingContext asset. Accepts exact object/package path or folder path plus exact name; bare names must resolve uniquely. Missing, ambiguous and other asset classes are rejected before deletion.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Exact asset name, never substring matching.\"},\"path\":{\"type\":\"string\",\"description\":\"Exact object/package path, or content folder when name is supplied.\"}}}},");
	J += TEXT("{\"name\":\"edit_mapping_context\",\"description\":\"Edit an InputMappingContext: add or remove key mappings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"add_mappings\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"remove_mappings\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}},");

	// === Generic Asset Tools ===
	J += TEXT("{\"name\":\"read_asset\",\"description\":\"Read any asset properties via reflection. Works with any UObject asset type.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"depth\":{\"type\":\"number\",\"description\":\"Recursion depth (default:1)\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"save_asset\",\"description\":\"Explicitly save one already-loaded project Blueprint, UserDefinedStruct or UserDefinedEnum package to disk (Windows). Load and compile separately first, resolving Blueprint compile warnings. Reports the completed save, file existence and target dirty state; a clean target previously saved to the same still-existing nonempty file is a no-op. Does not save dependencies or check out source-control files. Normal engine save hooks still run.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"assetPath\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact case-sensitive loaded /Game/Folder/Asset.Asset object path for a Blueprint, UserDefinedStruct or UserDefinedEnum. Windows only; load and compile separately before saving.\"}},\"required\":[\"assetPath\"],\"additionalProperties\":false}},");
	J += TEXT("{\"name\":\"find_assets\",\"description\":\"Search assets by name and optional class filter\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"searchTerm\":{\"type\":\"string\"},\"classFilter\":{\"type\":\"string\",\"description\":\"Loaded class name or exact class object path. Unknown or ambiguous classes are rejected; the filter is never silently dropped.\"},\"path\":{\"type\":\"string\"},\"maxResults\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000,\"default\":50}}}},");
	J += TEXT("{\"name\":\"get_asset_thumbnail\",\"description\":\"Render or read a cached asset thumbnail, resize to requested dimensions, and write a PNG at the returned absolute path. Failed pixel data or file writes are errors.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"width\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":256},\"height\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":4096,\"default\":256}},\"required\":[\"path\"]}},");

	// === Editor Tools ===
	J += TEXT("{\"name\":\"take_screenshot\",\"description\":\"Capture editor viewport screenshot\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"filename\":{\"type\":\"string\",\"description\":\"Optional PNG basename. The .png extension is appended if omitted; paths and other extensions are rejected.\"}}}},");
	J += TEXT("{\"name\":\"read_log\",\"description\":\"Read recent UE Output Log entries. Call this after a compile error or unexpected tool failure to get full diagnostic detail. Useful categories: 'LogK2Compiler' for Blueprint compile details, 'LogNwiroBP' for tool resolution details, 'LogBlueprint' for graph errors. Use severity:'Error' to filter to errors only.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"lines\":{\"type\":\"number\",\"description\":\"Number of lines (default:50)\"},\"severity\":{\"type\":\"string\",\"description\":\"Filter: Error/Warning/Log\"},\"category\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"play_in_editor\",\"description\":\"Start Play in Editor (PIE)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"stop_pie\",\"description\":\"Stop Play in Editor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	// NOT ADVERTISED (but still dispatchable — see DispatchToolImpl):
	// render_still_passes / get_render_job_status / cancel_render_job. The SCG
	// canvas calls them via bridge.executetool, which does not consult this
	// list. Kept from the chat model because a render takes ~10s, starves the
	// game thread and completes only through an async submit-then-poll dance.
	// To expose them later, restore the three entries; nothing else changes.

	// === Data Structure Tools ===
	J += TEXT("{\"name\":\"create_data_table\",\"description\":\"Create a DataTable asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"rowStruct\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"add_data_table_row\",\"description\":\"Add a row to a DataTable\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"table\":{\"type\":\"string\"},\"rowName\":{\"type\":\"string\"},\"values\":{\"type\":\"object\"}},\"required\":[\"table\",\"rowName\"]}},");
	J += TEXT("{\"name\":\"read_data_table\",\"description\":\"Read all rows from a DataTable\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"table\":{\"type\":\"string\"}},\"required\":[\"table\"]}},");
	J += TEXT("{\"name\":\"import_data_table_json\",\"description\":\"Replace all DataTable rows from a complete JSON array. This is replacement, not append. Validates every row on staging data; any validation error leaves the target unchanged. Does not save automatically.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"table\":{\"type\":\"string\"},\"json\":{\"type\":\"string\"}},\"required\":[\"table\",\"json\"]}},");
	J += TEXT("{\"name\":\"create_struct\",\"description\":\"Create a UserDefinedStruct with at least one field. Existing assets are rejected unchanged.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"pattern\":\"\\\\S\"},\"path\":{\"type\":\"string\",\"description\":\"Content folder; defaults to /Game/Data\"},\"fields\":{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"pattern\":\"\\\\S\",\"description\":\"Case-insensitively unique field name\"},\"type\":{\"description\":\"Case-insensitive primitives: bool/boolean, int/integer, float, double/real, string, name, text, vector, rotator, transform, color/linearcolor. References: class, object, struct, enum require subtype. Containers: none or array.\",\"oneOf\":[{\"type\":\"string\",\"minLength\":1},{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"base\":{\"type\":\"string\",\"minLength\":1},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Loaded Blueprint-eligible type name or exact object path. Ambiguous names, wrong kind, transient and unloaded types are rejected. Generated classes use their exact _C object path.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\"]}},\"required\":[\"base\"]}]},\"subtype\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Loaded Blueprint-eligible type name or exact object path. Ambiguous names, wrong kind, transient and unloaded types are rejected. Generated classes use their exact _C object path. Use only with a string type; otherwise put inside type.\"},\"container\":{\"type\":\"string\",\"enum\":[\"none\",\"array\"],\"description\":\"Use only with a string type; otherwise put inside type. Sets and maps are unsupported.\"}},\"required\":[\"name\",\"type\"]}}},\"required\":[\"name\",\"fields\"]}},");
	J += TEXT("{\"name\":\"create_enum\",\"description\":\"Create a UserDefinedEnum with at least one nonblank display name. Display names must be unique ignoring case. Existing assets are rejected unchanged.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"minLength\":1,\"pattern\":\"\\\\S\"},\"path\":{\"type\":\"string\",\"description\":\"Content folder; defaults to /Game/Data\"},\"values\":{\"type\":\"array\",\"minItems\":1,\"uniqueItems\":true,\"items\":{\"type\":\"string\",\"minLength\":1,\"pattern\":\"\\\\S\"}}},\"required\":[\"name\",\"values\"]}},");

	// === Animation Tools ===
	J += TEXT("{\"name\":\"create_montage\",\"description\":\"Create AnimMontage from skeleton or animation\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"animation\":{\"type\":\"string\"},\"skeleton\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_montage\",\"description\":\"Read AnimMontage structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_montage_section\",\"description\":\"Add section to montage\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"montage\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"startTime\":{\"type\":\"number\"}},\"required\":[\"montage\",\"name\",\"startTime\"]}},");
	J += TEXT("{\"name\":\"link_montage_sections\",\"description\":\"Link montage sections\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"montage\":{\"type\":\"string\"},\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"}},\"required\":[\"montage\",\"from\",\"to\"]}},");
	J += TEXT("{\"name\":\"add_montage_notify\",\"description\":\"Add notify to montage\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"montage\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"time\":{\"type\":\"number\"},\"duration\":{\"type\":\"number\"}},\"required\":[\"montage\",\"name\",\"time\"]}},");
	J += TEXT("{\"name\":\"create_anim_blueprint\",\"description\":\"Create AnimBlueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"skeleton\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\",\"skeleton\"]}},");
	J += TEXT("{\"name\":\"read_anim_blueprint\",\"description\":\"Read AnimBlueprint structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");

	// === Sequencer Tools ===
	J += TEXT("{\"name\":\"create_sequence\",\"description\":\"Create LevelSequence\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"fps\":{\"type\":\"number\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_sequence\",\"description\":\"Read an exact LevelSequence asset path or unique exact asset name. Returns possessable and spawnable bindings with each binding tracks and section counts, masterTracks, separate cameraCutTrack, and rational displayRate/fps. No substring fallback.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact LevelSequence asset path or unique exact asset name.\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_sequence_binding\",\"description\":\"Bind actor to sequence\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sequence\":{\"type\":\"string\"},\"actor\":{\"type\":\"string\"}},\"required\":[\"sequence\",\"actor\"]}},");
	J += TEXT("{\"name\":\"add_sequence_track\",\"description\":\"Add track to sequence binding. Types: Transform,Float,Bool,Audio,Event,CameraCut\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sequence\":{\"type\":\"string\"},\"binding\":{\"type\":\"string\"},\"trackType\":{\"type\":\"string\"}},\"required\":[\"sequence\",\"trackType\"]}},");
	J += TEXT("{\"name\":\"add_sequence_keyframe\",\"description\":\"Add transform keyframe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sequence\":{\"type\":\"string\"},\"binding\":{\"type\":\"string\"},\"frame\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},\"yaw\":{\"type\":\"number\"},\"roll\":{\"type\":\"number\"}},\"required\":[\"sequence\",\"binding\",\"frame\"]}},");
	J += TEXT("{\"name\":\"set_sequence_range\",\"description\":\"Set sequence playback range\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sequence\":{\"type\":\"string\"},\"startFrame\":{\"type\":\"number\"},\"endFrame\":{\"type\":\"number\"},\"fps\":{\"type\":\"number\"}},\"required\":[\"sequence\",\"startFrame\",\"endFrame\"]}},");

	// === AI Tools ===
	J += TEXT("{\"name\":\"create_behavior_tree\",\"description\":\"Create BehaviorTree asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"blackboard\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_behavior_tree\",\"description\":\"Read BehaviorTree structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"create_blackboard\",\"description\":\"Create Blackboard with keys\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"keys\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"}}}}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"edit_blackboard\",\"description\":\"Add/remove Blackboard keys\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"add_keys\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}},\"remove_keys\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"path\"]}},");

	// === Widget Blueprint Tools ===
	J += TEXT("{\"name\":\"create_widget_blueprint\",\"description\":\"Create UMG WidgetBlueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"rootWidget\":{\"type\":\"string\",\"description\":\"CanvasPanel(default),VerticalBox,HorizontalBox,Overlay\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_widget_blueprint\",\"description\":\"Read WidgetBlueprint tree\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_widget\",\"description\":\"Add widget to WidgetBlueprint. Classes: Button,TextBlock,Image,CanvasPanel,VerticalBox,HorizontalBox,Overlay,ScrollBox,Slider,CheckBox,ProgressBar,EditableTextBox,Border,Spacer,SizeBox\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"widgetClass\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"widgetClass\"]}},");
	J += TEXT("{\"name\":\"set_widget_property\",\"description\":\"Set property on a widget via reflection\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"widget\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"widget\",\"property\",\"value\"]}},");
	J += TEXT("{\"name\":\"render_widget_blueprint\",\"description\":\"Render a UMG WidgetBlueprint to PNG via FWidgetRenderer. Returns absolute file path. Optional width/height (default 1280x720) and saveTo (default ProjectSavedDir/NwiroWidgetRenders).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"width\":{\"type\":\"number\"},\"height\":{\"type\":\"number\"},\"saveTo\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");

	// === Niagara Tools ===
	J += TEXT("{\"name\":\"create_niagara_system\",\"description\":\"Create Niagara particle system\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_niagara_system\",\"description\":\"Read Niagara system structure\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"set_niagara_parameter\",\"description\":\"Set exposed parameter on Niagara system\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"system\":{\"type\":\"string\"},\"parameter\":{\"type\":\"string\"},\"value\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"system\",\"parameter\"]}},");

	// === State Tree Tools ===
	J += TEXT("{\"name\":\"create_state_tree\",\"description\":\"Create StateTree asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_state_tree\",\"description\":\"Read StateTree structure (states, tasks, transitions)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_state_tree_state\",\"description\":\"Add a state to a StateTree (top-level or as child of parent)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"stateTree\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"}},\"required\":[\"stateTree\",\"name\"]}},");

#if NWIRO_HAS_IK_TOOLS
	// === IK Rig Tools ===
	J += TEXT("{\"name\":\"create_ik_rig\",\"description\":\"Create IKRig asset with skeleton\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"skeleton\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_ik_rig\",\"description\":\"Read IKRig (goals, solvers, retarget chains)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_ik_goal\",\"description\":\"Add IK effector goal to IKRig\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"rig\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"bone\":{\"type\":\"string\"}},\"required\":[\"rig\",\"name\",\"bone\"]}},");
	J += TEXT("{\"name\":\"add_ik_solver\",\"description\":\"Add an IK solver to an IKRig using the UE5.7+ solver struct API. Types: FBIKSolver, LimbSolver, SetTransform, PoleSolver, or an exact loaded solver struct path.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"rig\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"}},\"required\":[\"rig\",\"type\"]}},");
	J += TEXT("{\"name\":\"add_retarget_chain\",\"description\":\"Add retarget chain to IKRig\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"rig\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"startBone\":{\"type\":\"string\"},\"endBone\":{\"type\":\"string\"}},\"required\":[\"rig\",\"name\",\"startBone\",\"endBone\"]}},");

	// === IK Retargeter Tools ===
	J += TEXT("{\"name\":\"create_ik_retargeter\",\"description\":\"Create IKRetargeter with source/target IKRigs\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"sourceIKRig\":{\"type\":\"string\"},\"targetIKRig\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"read_ik_retargeter\",\"description\":\"Read IKRetargeter source/target rigs and chain mappings from every retarget operation, identified by operation name and index.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"set_chain_mapping\",\"description\":\"Map an existing source chain to an existing target chain across matching retarget operations. sourceChain None clears the mapping. Invalid chains are rejected before mutation; unchanged mappings return changed:false.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"retargeter\":{\"type\":\"string\"},\"sourceChain\":{\"type\":\"string\"},\"targetChain\":{\"type\":\"string\"}},\"required\":[\"retargeter\",\"sourceChain\",\"targetChain\"]}},");

	// === Pose Search / Motion Matching Tools ===
	J += TEXT("{\"name\":\"create_pose_search_schema\",\"description\":\"Create PoseSearch schema for motion matching\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"skeleton\":{\"type\":\"string\",\"description\":\"Required exact Skeleton asset path; configured on the factory before creation.\"}},\"required\":[\"name\",\"skeleton\"]}},");
	J += TEXT("{\"name\":\"create_pose_search_database\",\"description\":\"Create PoseSearch database with schema\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"schema\":{\"type\":\"string\",\"description\":\"Required exact PoseSearchSchema asset path; configured on the factory before creation.\"}},\"required\":[\"name\",\"schema\"]}},");
	J += TEXT("{\"name\":\"read_pose_search_database\",\"description\":\"Read PoseSearch database (animations, schema)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"add_pose_search_animation\",\"description\":\"Add animation to PoseSearch database\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"database\":{\"type\":\"string\"},\"animation\":{\"type\":\"string\"}},\"required\":[\"database\",\"animation\"]}},");
#endif // NWIRO_HAS_IK_TOOLS

	// === BT / AnimBP / Niagara edit tools ===
	J += TEXT("{\"name\":\"add_behavior_tree_nodes\",\"description\":\"Unsupported in this build: BehaviorTree node authoring is not implemented. Returns unsupported_operation before any mutation; use the Unreal Behavior Tree editor.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"nodeType\":{\"type\":\"string\"},\"nodeClass\":{\"type\":\"string\"},\"parent\":{\"type\":\"string\"}},\"required\":[\"path\",\"nodeClass\"]}},");
	J += TEXT("{\"name\":\"add_anim_bp_state_machine\",\"description\":\"Add state machine to AnimBlueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"add_niagara_emitter\",\"description\":\"Add emitter to Niagara system\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"system\":{\"type\":\"string\"},\"emitter\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"system\",\"emitter\"]}},");

	// === Environment Tools ===
	J += TEXT("{\"name\":\"set_post_process\",\"description\":\"Configure PostProcessVolume: bloom, exposure, vignette, saturation, contrast\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"bloom_intensity\":{\"type\":\"number\"},\"auto_exposure_min\":{\"type\":\"number\"},\"auto_exposure_max\":{\"type\":\"number\"},\"vignette_intensity\":{\"type\":\"number\"},\"saturation\":{\"type\":\"number\"},\"contrast\":{\"type\":\"number\"},\"infinite_extent\":{\"type\":\"boolean\"}}}},");
	J += TEXT("{\"name\":\"set_fog\",\"description\":\"Configure ExponentialHeightFog: density, falloff, distance, volumetric\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"density\":{\"type\":\"number\"},\"height_falloff\":{\"type\":\"number\"},\"start_distance\":{\"type\":\"number\"},\"volumetric\":{\"type\":\"boolean\"}}}},");
	J += TEXT("{\"name\":\"set_sky_atmosphere\",\"description\":\"Apply a validated batch through public native SkyAtmosphere setters to exactly one existing actor component in the idle editor world. Requires a movable or stationary registered component. No automatic save; returns actual component values. Exact native property names only.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact full object path, actor name, or unique actor label in the editor world.\"},\"properties\":{\"type\":\"object\",\"minProperties\":1,\"properties\":{\"BottomRadius\":{\"type\":\"number\",\"minimum\":0.1,\"maximum\":10000},\"AtmosphereHeight\":{\"type\":\"number\",\"minimum\":0.1,\"maximum\":3.4028234663852886e+38},\"MultiScatteringFactor\":{\"type\":\"number\",\"minimum\":0,\"maximum\":100},\"RayleighScatteringScale\":{\"type\":\"number\",\"minimum\":0,\"maximum\":3.4028234663852886e+38},\"RayleighExponentialDistribution\":{\"type\":\"number\",\"minimum\":0.001,\"maximum\":3.4028234663852886e+38},\"MieScatteringScale\":{\"type\":\"number\",\"minimum\":0,\"maximum\":3.4028234663852886e+38},\"MieAbsorptionScale\":{\"type\":\"number\",\"minimum\":0,\"maximum\":3.4028234663852886e+38},\"MieAnisotropy\":{\"type\":\"number\",\"minimum\":0,\"maximum\":0.999},\"MieExponentialDistribution\":{\"type\":\"number\",\"minimum\":0.001,\"maximum\":3.4028234663852886e+38},\"OtherAbsorptionScale\":{\"type\":\"number\",\"minimum\":0,\"maximum\":3.4028234663852886e+38},\"AerialPespectiveViewDistanceScale\":{\"type\":\"number\",\"minimum\":0,\"maximum\":3.4028234663852886e+38},\"AerialPerspectiveStartDepth\":{\"type\":\"number\",\"minimum\":0.001,\"maximum\":3.4028234663852886e+38},\"HeightFogContribution\":{\"type\":\"number\",\"minimum\":0,\"maximum\":3.4028234663852886e+38},\"TransmittanceMinLightElevationAngle\":{\"type\":\"number\",\"minimum\":-90,\"maximum\":90},\"RayleighScattering\":{\"type\":\"object\",\"description\":\"Linear RGB channels; preserves alpha.\",\"properties\":{\"r\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"g\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"b\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38}},\"required\":[\"r\",\"g\",\"b\"],\"additionalProperties\":false},\"MieScattering\":{\"type\":\"object\",\"description\":\"Linear RGB channels; preserves alpha.\",\"properties\":{\"r\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"g\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"b\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38}},\"required\":[\"r\",\"g\",\"b\"],\"additionalProperties\":false},\"MieAbsorption\":{\"type\":\"object\",\"description\":\"Linear RGB channels; preserves alpha.\",\"properties\":{\"r\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"g\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"b\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38}},\"required\":[\"r\",\"g\",\"b\"],\"additionalProperties\":false},\"OtherAbsorption\":{\"type\":\"object\",\"description\":\"Linear RGB channels; preserves alpha.\",\"properties\":{\"r\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"g\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"b\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38}},\"required\":[\"r\",\"g\",\"b\"],\"additionalProperties\":false},\"SkyLuminanceFactor\":{\"type\":\"object\",\"description\":\"Linear RGB channels; preserves alpha.\",\"properties\":{\"r\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"g\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38},\"b\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1e+38}},\"required\":[\"r\",\"g\",\"b\"],\"additionalProperties\":false},\"GroundAlbedo\":{\"type\":\"object\",\"description\":\"RGB byte channels; preserves alpha.\",\"properties\":{\"r\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"g\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"b\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},\"required\":[\"r\",\"g\",\"b\"],\"additionalProperties\":false}},\"additionalProperties\":false}},\"required\":[\"actor\",\"properties\"],\"additionalProperties\":false}},");
	J += TEXT("{\"name\":\"set_light_properties\",\"description\":\"Set light properties: intensity, color, temperature, shadows, radius, cone angles\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"intensity\":{\"type\":\"number\"},\"color\":{\"type\":\"object\"},\"temperature\":{\"type\":\"number\"},\"cast_shadows\":{\"type\":\"boolean\"},\"source_radius\":{\"type\":\"number\"},\"attenuation_radius\":{\"type\":\"number\"},\"inner_cone_angle\":{\"type\":\"number\"},\"outer_cone_angle\":{\"type\":\"number\"}},\"required\":[\"actor\"]}},");

	// === Physics Tools ===
	J += TEXT("{\"name\":\"set_physics_simulation\",\"description\":\"Enable/configure physics on actor: simulate, gravity, mass, damping\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"simulate\":{\"type\":\"boolean\"},\"gravity\":{\"type\":\"boolean\"},\"mass\":{\"type\":\"number\"},\"linear_damping\":{\"type\":\"number\"},\"angular_damping\":{\"type\":\"number\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_collision_profile\",\"description\":\"Set collision profile on actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"profile\":{\"type\":\"string\"}},\"required\":[\"actor\",\"profile\"]}},");
	J += TEXT("{\"name\":\"add_physics_constraint\",\"description\":\"Create physics constraint between two actors. Types: Fixed, Hinge, BallSocket\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor1\":{\"type\":\"string\"},\"actor2\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"}},\"required\":[\"actor1\",\"actor2\"]}},");
	J += TEXT("{\"name\":\"get_physics_info\",\"description\":\"Get physics state of an actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");

	// === Spline Tools ===
	J += TEXT("{\"name\":\"create_spline_actor\",\"description\":\"Create spline actor with initial points\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},\"points\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}}}},");
	J += TEXT("{\"name\":\"add_spline_point\",\"description\":\"Add point to spline\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_spline_point\",\"description\":\"Move a spline point\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"index\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"actor\",\"index\"]}},");
	J += TEXT("{\"name\":\"remove_spline_point\",\"description\":\"Remove spline point by index\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"index\":{\"type\":\"number\"}},\"required\":[\"actor\",\"index\"]}},");
	J += TEXT("{\"name\":\"get_spline_info\",\"description\":\"Get spline details: points, length, closed state\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_spline_closed\",\"description\":\"Toggle spline closed loop\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"closed\":{\"type\":\"boolean\"}},\"required\":[\"actor\",\"closed\"]}},");
	J += TEXT("{\"name\":\"set_spline_point_type\",\"description\":\"Set spline point type: Linear, Curve, Constant, CurveClamped\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"index\":{\"type\":\"number\"},\"type\":{\"type\":\"string\"}},\"required\":[\"actor\",\"index\",\"type\"]}},");

	// === Navigation Tools ===
	J += TEXT("{\"name\":\"build_navigation\",\"description\":\"Build navmesh. Requires NavMeshBoundsVolume in level.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"query_navigation_path\",\"description\":\"Find navigation path between two points\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"startX\":{\"type\":\"number\"},\"startY\":{\"type\":\"number\"},\"startZ\":{\"type\":\"number\"},\"endX\":{\"type\":\"number\"},\"endY\":{\"type\":\"number\"},\"endZ\":{\"type\":\"number\"}},\"required\":[\"startX\",\"startY\",\"startZ\",\"endX\",\"endY\",\"endZ\"]}},");
	J += TEXT("{\"name\":\"get_navigation_info\",\"description\":\"Get navmesh build status and bounds volume count\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Audio Tools ===
	J += TEXT("{\"name\":\"spawn_sound\",\"description\":\"Spawn AmbientSound actor with sound asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"sound\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"label\":{\"type\":\"string\"},\"volume\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"}},\"required\":[\"sound\"]}},");
	J += TEXT("{\"name\":\"set_audio_properties\",\"description\":\"Set audio component properties on actor\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"volume\":{\"type\":\"number\"},\"pitch\":{\"type\":\"number\"},\"autoActivate\":{\"type\":\"boolean\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"get_sound_info\",\"description\":\"Get sound asset details: duration, channels, sample rate\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");

	// === Game Framework Tools ===
	J += TEXT("{\"name\":\"create_game_mode\",\"description\":\"Create GameMode Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_player_controller\",\"description\":\"Create PlayerController Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_game_state\",\"description\":\"Create GameState Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_player_state\",\"description\":\"Create PlayerState Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_hud\",\"description\":\"Create HUD Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"get_game_framework_info\",\"description\":\"Get current game framework configuration\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Build / Validation Tools ===
	J += TEXT("{\"name\":\"get_project_info\",\"description\":\"Get project name, engine version, paths\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"list_project_modules\",\"description\":\"List loaded project modules\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"validate_assets\",\"description\":\"Run basic asset validation\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_map_check_errors\",\"description\":\"Run the editor Map Check and return its findings. success:true means the CHECK RAN, not that the map is clean \u2014 read errorCount/warningCount and messages[]. truncated:true means the log buffer held fewer entries than the counts.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_build_configuration\",\"description\":\"Get current build config and platform\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Resources ===
	J += TEXT("{\"name\":\"list_resources\",\"description\":\"List available MCP resources (read-only context data)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"read_resource\",\"description\":\"Read a resource by URI. URIs: nwiro://project/info, nwiro://level/current, nwiro://editor/selection, nwiro://editor/performance, nwiro://editor/log, nwiro://editor/viewport\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"uri\":{\"type\":\"string\"}},\"required\":[\"uri\"]}},");

	// === Level Tools ===
	J += TEXT("{\"name\":\"new_level\",\"description\":\"Create a new empty level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"template\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"open_level\",\"description\":\"Open an existing level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"save_level\",\"description\":\"Save current level\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_level_info\",\"description\":\"Get level metadata: name, actors, streaming levels, bounds\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Macro Tools ===
	J += TEXT("{\"name\":\"create_basic_level\",\"description\":\"Create basic level with floor, sun, skylight, player start\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"create_light_rig\",\"description\":\"Create 3-point light rig: key, fill, rim + skylight\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"radius\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"create_grid_layout\",\"description\":\"Arrange meshes in a grid pattern\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mesh\":{\"type\":\"string\"},\"rows\":{\"type\":\"number\"},\"cols\":{\"type\":\"number\"},\"spacing\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"create_ring_layout\",\"description\":\"Arrange meshes in a circle/ring\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mesh\":{\"type\":\"string\"},\"count\":{\"type\":\"number\"},\"radius\":{\"type\":\"number\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"faceCenter\":{\"type\":\"boolean\"}}}},");

	// === Landscape Tools ===
	J += TEXT("{\"name\":\"create_landscape\",\"description\":\"Create a landscape from an EXPLICIT component layout. Optionally import a 16-bit grayscale heightmap PNG as its geometry (heightmapPath); omit for flat. modify=true re-imports onto an existing landscape. ensureLayersOnly=true (with landscapeName) does NOT create/import — it only ensures the type-correct Patches edit layer exists on that landscape (M2 existing-flat base). Layout is explicit: sectionSize (7/15/31/63/127/255) + numSubsections (1 or 2) + componentCount; resolution = componentCount*numSubsections*sectionSize+1. scale defaults [100,100,100], location defaults to origin. (The caller/backend computes the layout, scale and centered location.)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"heightmapPath\":{\"type\":\"string\"},\"sectionSize\":{\"type\":\"number\"},\"numSubsections\":{\"type\":\"number\"},\"componentCount\":{\"type\":\"number\"},\"scale\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"location\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"material\":{\"type\":\"string\"},\"editLayers\":{\"type\":\"boolean\"},\"ensureLayersOnly\":{\"type\":\"boolean\"},\"modify\":{\"type\":\"boolean\"},\"landscapeName\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"set_landscape_material\",\"description\":\"Set material on landscape\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"material\":{\"type\":\"string\"}},\"required\":[\"material\"]}},");
	J += TEXT("{\"name\":\"get_landscape_info\",\"description\":\"Get landscape details. Optional landscapeName targets a specific landscape by label; omitted returns the first landscape in the level.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"landscapeName\":{\"type\":\"string\"}}}},");

	// === Foliage Tools ===
	J += TEXT("{\"name\":\"add_foliage_type\",\"description\":\"Register mesh as foliage type at location (default origin), supporting ordinary and World Partition levels.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mesh\":{\"type\":\"string\",\"minLength\":1,\"description\":\"StaticMesh asset path.\"},\"location\":{\"anyOf\":[{\"type\":\"array\",\"minItems\":3,\"maxItems\":3,\"items\":{\"type\":\"number\"}},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1}]},\"center\":{\"anyOf\":[{\"type\":\"array\",\"minItems\":3,\"maxItems\":3,\"items\":{\"type\":\"number\"}},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1}],\"description\":\"Compatibility alias for location; conflicting aliases reject.\"},\"x\":{\"type\":\"number\",\"description\":\"Compatibility location component; conflicting aliases reject.\"},\"y\":{\"type\":\"number\",\"description\":\"Compatibility location component; conflicting aliases reject.\"},\"z\":{\"type\":\"number\",\"description\":\"Compatibility location component; conflicting aliases reject.\"}},\"required\":[\"mesh\"]}},");
	J += TEXT("{\"name\":\"paint_foliage\",\"description\":\"Paint foliage around location (default origin), placing each instance in its World Partition cell. count is an exact integer; invalid requests are rejected before mutation.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"mesh\":{\"type\":\"string\",\"minLength\":1,\"description\":\"StaticMesh asset path.\"},\"location\":{\"anyOf\":[{\"type\":\"array\",\"minItems\":3,\"maxItems\":3,\"items\":{\"type\":\"number\"}},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1}]},\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000,\"default\":10},\"radius\":{\"type\":\"number\",\"minimum\":0,\"default\":500},\"center\":{\"anyOf\":[{\"type\":\"array\",\"minItems\":3,\"maxItems\":3,\"items\":{\"type\":\"number\"}},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1},{\"type\":\"object\",\"properties\":{\"X\":{\"type\":\"number\"},\"Y\":{\"type\":\"number\"},\"Z\":{\"type\":\"number\"}},\"additionalProperties\":false,\"minProperties\":1}],\"description\":\"Compatibility alias for location; conflicting aliases reject.\"},\"x\":{\"type\":\"number\",\"description\":\"Compatibility location component; conflicting aliases reject.\"},\"y\":{\"type\":\"number\",\"description\":\"Compatibility location component; conflicting aliases reject.\"},\"z\":{\"type\":\"number\",\"description\":\"Compatibility location component; conflicting aliases reject.\"}},\"required\":[\"mesh\"]}},");
	J += TEXT("{\"name\":\"erase_foliage\",\"description\":\"Erase foliage instances\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_foliage_stats\",\"description\":\"Get foliage instance counts per type\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");

	// === Networking Tools ===
	J += TEXT("{\"name\":\"get_replication_info\",\"description\":\"Get actor replication settings\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_replication_settings\",\"description\":\"Configure actor replication\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"replicates\":{\"type\":\"boolean\"},\"replicateMovement\":{\"type\":\"boolean\"},\"netUpdateFrequency\":{\"type\":\"number\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"set_net_dormancy\",\"description\":\"Set network dormancy mode: Awake, DormantAll, DormantPartial, Initial, Never\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"}},\"required\":[\"actor\",\"mode\"]}},");

	// === World Partition Tools ===
	J += TEXT("{\"name\":\"get_world_partition_info\",\"description\":\"Get World Partition status\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"load_world_partition_region\",\"description\":\"Load WP editor cells in region\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"minX\":{\"type\":\"number\"},\"minY\":{\"type\":\"number\"},\"minZ\":{\"type\":\"number\"},\"maxX\":{\"type\":\"number\"},\"maxY\":{\"type\":\"number\"},\"maxZ\":{\"type\":\"number\"}}}},");

	// === Undo/Redo ===
	J += TEXT("{\"name\":\"undo\",\"description\":\"Undo editor operations (max 50)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"redo\",\"description\":\"Redo editor operations (max 50)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"number\"}}}},");

	// === GAS Tools ===
	J += TEXT("{\"name\":\"create_gameplay_ability\",\"description\":\"Create Gameplay Ability Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_gameplay_effect\",\"description\":\"Create Gameplay Effect Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"create_attribute_set\",\"description\":\"Create Attribute Set Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"list_gameplay_abilities\",\"description\":\"List Gameplay Ability assets\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"list_gameplay_effects\",\"description\":\"List Gameplay Effect assets\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"list_attribute_sets\",\"description\":\"List Attribute Set assets\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"get_gas_info\",\"description\":\"Inspect an exact actor path/name/unique label in PIE when active, otherwise editor world, or an exact Blueprint asset path. Resolves actual native ASC state using optional public native getters. Blueprint current defaults and uninstantiated SCS declarations are separate; never spawns or compiles to inspect. gasAvailable/capabilities identify missing optional support; omitted fields and null presence are unknown, not empty. Attribute sets use first-match lookup per loaded class, so additional same-class instances can be omitted; attributeSetInventoryComplete and component complete remain false.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\",\"minLength\":1},\"path\":{\"type\":\"string\",\"minLength\":1}},\"anyOf\":[{\"required\":[\"actor\"]},{\"required\":[\"path\"]}]}},");

	// === PCG Tools (default — Extended PCG extension uses its own pipeline) ===
	J += TEXT("{\"name\":\"create_pcg_graph\",\"description\":\"Create an empty UPCGGraph asset. Step 1 of placing procedural content.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Asset name (no extension)\"},\"path\":{\"type\":\"string\",\"description\":\"Folder, default /Game/PCG\"}},\"required\":[\"name\"]}},");
	J += TEXT("{\"name\":\"find_pcg_graphs\",\"description\":\"List existing PCG graph assets in the project.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}}}},");
	J += TEXT("{\"name\":\"spawn_pcg_volume\",\"description\":\"Spawn a PCGVolume actor in the current level pointing at a UPCGGraph asset and trigger Generate(). Step 2 after create_pcg_graph.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"graph\":{\"type\":\"string\",\"description\":\"Full UPCGGraph asset path, e.g. /Game/PCG/Forest\"},\"label\":{\"type\":\"string\",\"description\":\"Actor label, default PCG_Volume\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"scaleX\":{\"type\":\"number\",\"description\":\"Default 20 (=2000cm)\"},\"scaleY\":{\"type\":\"number\"},\"scaleZ\":{\"type\":\"number\",\"description\":\"Default 5 (=500cm)\"}},\"required\":[\"graph\"]}},");
	J += TEXT("{\"name\":\"pcg_generate\",\"description\":\"Force a PCGVolume in the level to re-run its graph.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\",\"description\":\"Label of the PCGVolume actor\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"add_pcg_node\",\"description\":\"Add a node of the given UPCGSettings subclass to a UPCGGraph. Friendly names like StaticMeshSpawner / SurfaceSampler / SelfPruning / CopyPoints / DensityFilter are resolved via fuzzy class lookup. Use this to populate a graph before pcg_generate.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"graph\":{\"type\":\"string\",\"description\":\"Full UPCGGraph asset path, e.g. /Game/PCG/Forest\"},\"node\":{\"type\":\"string\",\"description\":\"Settings class friendly name, e.g. StaticMeshSpawner\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}},\"required\":[\"graph\",\"node\"]}},");

	// === PIE Runtime Control ===
	J += TEXT("{\"name\":\"pie_teleport_actor\",\"description\":\"Teleport actor during PIE gameplay\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"actor\",\"x\",\"y\",\"z\"]}},");
	J += TEXT("{\"name\":\"pie_spawn_actor\",\"description\":\"Spawn actor in PIE world\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"class\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"pie_destroy_actor\",\"description\":\"Destroy actor in PIE world\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"pie_get_property\",\"description\":\"Read actor property during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"}},\"required\":[\"actor\",\"property\"]}},");
	J += TEXT("{\"name\":\"pie_set_property\",\"description\":\"Set actor property during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"property\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"actor\",\"property\",\"value\"]}},");
	J += TEXT("{\"name\":\"pie_set_blackboard_key\",\"description\":\"Set AI Blackboard key during PIE. Types: bool,int,float,string,vector,object\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},\"value\":{},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"}},\"required\":[\"actor\",\"key\"]}},");
	J += TEXT("{\"name\":\"pie_get_blackboard_key\",\"description\":\"Read AI Blackboard key during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"key\":{\"type\":\"string\"}},\"required\":[\"actor\",\"key\"]}},");
	J += TEXT("{\"name\":\"pie_move_ai_to\",\"description\":\"Command a pawn to walk to a location during PIE through its AIController (needs a NavMesh). Pass an AI-controlled or unpossessed pawn, or the AIController itself. Cannot move the player's pawn; use pie_teleport_actor to reposition it.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"},\"z\":{\"type\":\"number\"},\"acceptanceRadius\":{\"type\":\"number\"}},\"required\":[\"actor\",\"x\",\"y\",\"z\"]}},");
	J += TEXT("{\"name\":\"pie_stop_ai\",\"description\":\"Stop AI movement and behavior tree during PIE\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"actor\":{\"type\":\"string\"}},\"required\":[\"actor\"]}},");
	J += TEXT("{\"name\":\"pie_get_game_state\",\"description\":\"Get PIE game state: time, player position, actor/pawn count\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},");
	J += TEXT("{\"name\":\"pie_list_actors\",\"description\":\"List actors in PIE world with class filter\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"classFilter\":{\"type\":\"string\"},\"limit\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"pie_console_command\",\"description\":\"Execute console command in PIE world\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}},");

	// === Blueprint Debugger ===
	J += TEXT("{\"name\":\"bp_get_compile_errors\",\"description\":\"Get Blueprint compile errors and warnings with node/graph info\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"],\"x-nwiro-verification\":{\"version\":1,\"noSave\":true,\"canonicalTarget\":true}}},");
	J += TEXT("{\"name\":\"bp_set_breakpoint\",\"description\":\"Set breakpoint on a Blueprint node\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"node\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"node\"]}},");
	J += TEXT("{\"name\":\"bp_remove_breakpoint\",\"description\":\"Remove breakpoint from a Blueprint node\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"node\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"node\"]}},");
	J += TEXT("{\"name\":\"bp_list_breakpoints\",\"description\":\"List all breakpoints in a Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_get_watch_values\",\"description\":\"Get watched pin values in Blueprint (debug)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_add_watch\",\"description\":\"Add watch on a Blueprint node pin\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"},\"node\":{\"type\":\"string\"},\"pin\":{\"type\":\"string\"}},\"required\":[\"blueprint\",\"node\",\"pin\"]}},");

	// === Blueprint Error Fixer ===
	J += TEXT("{\"name\":\"bp_fix_broken_references\",\"description\":\"Find and remove broken pin connections in Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_fix_deprecated_nodes\",\"description\":\"Refresh deprecated nodes to newer versions\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_refresh_all_nodes\",\"description\":\"Refresh all nodes and recompile Blueprint\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");
	J += TEXT("{\"name\":\"bp_find_unconnected_pins\",\"description\":\"Find output exec pins with no connections (logic flow gaps)\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"blueprint\":{\"type\":\"string\"}},\"required\":[\"blueprint\"]}},");

	// === Asset Dependency ===
	J += TEXT("{\"name\":\"get_asset_references\",\"description\":\"Get all assets this asset depends on\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Exact existing asset package path (/Game/Folder/A) or object path (/Game/Folder/A.A). Missing or unavailable dependency data is an error; no asset loading.\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"get_asset_referencers\",\"description\":\"Get all assets that reference this asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Exact existing asset package path (/Game/Folder/A) or object path (/Game/Folder/A.A). Missing or unavailable dependency data is an error; no asset loading.\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"find_orphan_assets\",\"description\":\"Find assets not referenced by anything\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Existing content folder, not an asset object path. Defaults to /Game.\"},\"maxResults\":{\"type\":\"number\"}}}},");
	J += TEXT("{\"name\":\"find_circular_dependencies\",\"description\":\"Detect circular dependency chains from an asset\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},");
	J += TEXT("{\"name\":\"get_dependency_tree\",\"description\":\"Get full dependency tree as recursive JSON\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Exact existing asset package path (/Game/Folder/A) or object path (/Game/Folder/A.A). Missing or unavailable dependency data is an error; no asset loading.\"},\"depth\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":64,\"default\":3,\"description\":\"Maximum traversal depth. Terminal depth boundaries and cycles are marked explicitly.\"}},\"required\":[\"path\"]}},");

	// === 3D Generation Tools ===
	J += TEXT("{\"name\":\"generate_3d_model_meshy\",\"description\":\"Generate a 3D model from a text description using Meshy AI. Requires 'Meshy 3D' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Detailed description of the 3D model to generate\"},\"topology\":{\"type\":\"string\",\"enum\":[\"quad\",\"triangle\"]},\"target_polycount\":{\"type\":\"integer\",\"minimum\":100,\"maximum\":300000},\"should_remesh\":{\"type\":\"boolean\"},\"target_formats\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"glb\",\"fbx\",\"obj\"]},\"minItems\":1,\"maxItems\":3}},\"required\":[\"prompt\"]}},");
	J += TEXT("{\"name\":\"generate_texture_meshy\",\"description\":\"Texture an existing 3D model using Meshy AI. Requires 'Meshy 3D' extension enabled. Provide model_id (the Meshy task ID of a finished model or Refine; a Texture result cannot be textured again) or model_url (GLB/FBX/OBJ URL), and a text prompt describing the desired texture. model_id is used when both are given.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"model_url\":{\"type\":\"string\",\"description\":\"URL to a 3D model file (GLB/FBX/OBJ)\"},\"model_id\":{\"type\":\"string\",\"description\":\"Meshy task ID of a finished model or Refine (not of a Texture result)\"},\"prompt\":{\"type\":\"string\",\"maxLength\":800,\"description\":\"Description of the desired texture, up to 800 characters\"}},\"required\":[\"prompt\"]}},");
	J += TEXT("{\"name\":\"generate_3d_model_tripo\",\"description\":\"Generate a 3D model from a text description using Tripo3D. Requires 'Tripo 3D' extension enabled for this chat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Detailed description of the 3D model to generate\"}},\"required\":[\"prompt\"]}},");

	// === ElevenLabs Audio Tools ===
	J += TEXT("{\"name\":\"generate_voice_elevenlabs\",\"description\":\"Generate text-to-speech audio (dialogue, NPC lines, narration) using ElevenLabs. Requires 'ElevenLabs' extension enabled. The audio is saved to the project's Saved/Nwiro/audio folder and emitted as an elevenlabs_task event so the chat panel can preview/import it.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"Text to convert to speech\"},\"voiceId\":{\"type\":\"string\",\"description\":\"ElevenLabs voice ID (use list_voices_elevenlabs to discover)\"},\"modelId\":{\"type\":\"string\",\"description\":\"Optional. Defaults to eleven_multilingual_v2\"},\"languageCode\":{\"type\":\"string\",\"description\":\"Optional ISO language code (e.g. en, tr)\"}},\"required\":[\"text\",\"voiceId\"]}},");
	J += TEXT("{\"name\":\"generate_sfx_elevenlabs\",\"description\":\"Generate a sound effect from a text description using ElevenLabs sound-generation. Requires 'ElevenLabs' extension enabled.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Description of the sound (e.g. 'footsteps on snow')\"},\"durationSeconds\":{\"type\":\"number\",\"description\":\"Optional. 0.5-30, omit for auto.\"},\"promptInfluence\":{\"type\":\"number\",\"description\":\"Optional. 0-1, default 0.3\"},\"loop\":{\"type\":\"boolean\",\"description\":\"Optional. Generate a seamlessly looping clip.\"}},\"required\":[\"prompt\"]}},");
	J += TEXT("{\"name\":\"generate_music_elevenlabs\",\"description\":\"Generate music or longer-form audio (3s-10min) from a text description using ElevenLabs Music. Requires 'ElevenLabs' extension enabled.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Description of the music to generate\"},\"durationMs\":{\"type\":\"integer\",\"description\":\"Optional. 3000-600000 milliseconds.\"},\"instrumental\":{\"type\":\"boolean\",\"description\":\"Optional. Force instrumental (no vocals).\"}},\"required\":[\"prompt\"]}},");
	J += TEXT("{\"name\":\"list_voices_elevenlabs\",\"description\":\"List ElevenLabs voices available to the user. Returns {voices: [{voice_id, name, category, labels, preview_url}], has_more, total_count, next_page_token}. Requires 'ElevenLabs' extension enabled.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"search\":{\"type\":\"string\"},\"category\":{\"type\":\"string\",\"description\":\"premade | cloned | generated | professional\"},\"pageSize\":{\"type\":\"integer\"}},\"required\":[]}},");

	// === fal.ai Material Tools ===
	J += TEXT("{\"name\":\"generate_material_fal\",\"description\":\"Generate a seamless tileable PBR material (basecolor, normal, roughness, metalness, height) from a text prompt or reference image using fal-ai/patina/material. Requires 'fal.ai' extension enabled. The resulting maps appear in the fal.ai panel where the user can preview and import them as UTexture2D assets.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Material description (e.g. 'weathered copper roof', 'mossy stone wall')\"},\"imageUrl\":{\"type\":\"string\",\"description\":\"Optional reference image URL for image-to-material\"},\"strength\":{\"type\":\"number\",\"description\":\"Optional 0-1, default 0.6\"},\"imageSize\":{\"type\":\"string\",\"enum\":[\"square_hd\",\"square\",\"portrait_4_3\",\"portrait_16_9\",\"landscape_4_3\",\"landscape_16_9\"]},\"numInferenceSteps\":{\"type\":\"integer\",\"description\":\"Optional 1-8, default 8\"},\"numImages\":{\"type\":\"integer\",\"description\":\"Optional 1-4, default 1\"},\"seed\":{\"type\":\"integer\",\"description\":\"Optional\"},\"maps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional subset of [basecolor, normal, roughness, metalness, height]\"},\"upscaleFactor\":{\"type\":\"integer\",\"description\":\"Optional 0|2|4, default 0\"},\"outputFormat\":{\"type\":\"string\",\"enum\":[\"png\",\"jpeg\",\"webp\"]},\"tilingMode\":{\"type\":\"string\",\"enum\":[\"both\",\"horizontal\",\"vertical\"]}},\"required\":[\"prompt\"]}},");

	// === Scene Gen ===
	// Advertised here, but run by the Nwiro app: the Scene Gen workflows execute
	// in the Nwiro panel, so these entries only tell the assistant that the tools
	// exist and what they accept. There is intentionally no handler for either
	// name in DispatchToolImpl below.
	//
	// No field is required, and `settings` is intentionally open-ended. The
	// Scene Gen canvas is a shared workspace - the user sets it up too - so
	// whether a prompt is supplied is what separates "set up a new generation"
	// from "run what is already on the canvas". That guidance, and the list of
	// settings currently accepted, are delivered with the assistant's Scene Gen
	// instructions rather than pinned here, so this declaration stays stable as
	// the workflow gains options.
	J += TEXT("{\"name\":\"generate_scene_image\",\"description\":\"Render the user's current Unreal scene and generate a styled image from it, using the scene's depth and edges so the result matches the real geometry and camera. With a prompt, sets it up (or updates the canvas the user already has open) and runs. Without a prompt, runs whatever is currently set up in the Scene Gen canvas, unchanged. Requires 'Scene Gen' extension enabled for this chat. Blocks until the image is ready (typically under a minute); the image appears in the Scene Gen canvas panel.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"What the image should look like: style, lighting, materials, mood. Geometry and camera come from the scene render, not from this text. Include this whenever the user gives ANY style or change words. Omit it ONLY for 'run this' / 'run the current canvas' / 'start it as-is'. Never send an empty string.\"},\"camera\":{\"type\":\"string\",\"description\":\"Camera actor label to render from. Defaults to 'viewport'. Omit to leave the camera the user already chose.\"},\"settings\":{\"type\":\"object\",\"description\":\"Optional generation settings. The valid keys are listed in the Scene Gen section of your instructions - do not invent keys.\"}},\"required\":[]}},");

	// The video half of Scene Gen. Same arrangement: declared here, executed in
	// the app. Two differences from the still tool are worth knowing before
	// touching this line, and BOTH are described properly in the assistant's
	// Scene Gen instructions rather than here (that text deploys in minutes; this
	// costs a rebuild and a reinstall per engine version):
	//   - it cannot build a setup from nothing, because it animates a generation
	//     the user already made and only they can choose which one;
	//   - it runs for MINUTES, so the call routinely returns before the video is
	//     finished, which is normal rather than a failure.
	// `settings` is open-ended here for the same reason as the still tool: the
	// keys the video workflow accepts are delivered with the instructions, so a
	// setting can be added, renamed or withdrawn without a rebuild. Only the
	// field's EXISTENCE costs one, which is why it is declared before every key
	// that will eventually live in it.
	J += TEXT("{\"name\":\"generate_scene_video\",\"description\":\"Animate a still the user has already generated in Scene Gen along the camera move of a Level Sequence, using a depth video rendered from that sequence. Requires a Scene Gen canvas that already has a picked generation and a chosen sequence - it cannot create one from nothing. With a prompt, updates the style on their canvas and runs; without a prompt, runs what is already set up. Requires 'Scene Gen' extension enabled for this chat. Takes several minutes and usually returns before the video is finished; the video appears in the Scene Gen canvas panel.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\",\"description\":\"Style, lighting and mood words for the video. The scene's contents come from the user's still and the motion from the sequence, not from this text. Include this whenever the user gives ANY style or change words. Omit it ONLY for 'run this' / 'run the current canvas'. Never send an empty string.\"},\"sequence\":{\"type\":\"string\",\"description\":\"Which Level Sequence to film: its name as the user said it, or a full /Game/... asset path. Matched exactly against the project's sequences. Omit to keep whatever the canvas already has.\"},\"settings\":{\"type\":\"object\",\"description\":\"Optional generation settings. The valid keys are listed in the Scene Gen section of your instructions - do not invent keys.\"}},\"required\":[]}}");

	// Third-party tool sources — append their namespaced defs before the array
	// closes. Models get the enabled set; the settings UI gets everything with
	// per-def disabled flags so a disabled source keeps its re-enable toggle.
	if (bForSettings)
	{
		FNwiroToolRegistry::AppendAllProviderDefsWithState(J);
	}
	else
	{
		FNwiroToolRegistry::AppendEnabledProviderDefs(J);
	}

	J += TEXT("]");
	// Advertise exactly one collision-free provider variant. Legacy unsuffixed
	// names remain dispatcher aliases but are never exposed to new clients.
	extern FString LoadNwiroSecret(const FString& Key);
	auto ProviderSuffix = [](const TCHAR* Provider) {
		const FString Mode = LoadNwiroSecret(FString::Printf(TEXT("NWIRO_PROVIDER_MODE_%s"), Provider));
		return Mode.Equals(TEXT("managed"), ESearchCase::IgnoreCase) ? TEXT("_managed") : TEXT("_byok");
	};
	const FString MeshySuffix = ProviderSuffix(TEXT("MESHY"));
	const FString TripoSuffix = ProviderSuffix(TEXT("TRIPO"));
	const FString ElevenSuffix = ProviderSuffix(TEXT("ELEVENLABS"));
	const FString FalSuffix = ProviderSuffix(TEXT("FAL"));
	J.ReplaceInline(TEXT("\"name\":\"generate_3d_model_meshy\""), *FString::Printf(TEXT("\"name\":\"generate_3d_model_meshy%s\""), *MeshySuffix));
	J.ReplaceInline(TEXT("\"name\":\"generate_texture_meshy\""), *FString::Printf(TEXT("\"name\":\"generate_texture_meshy%s\""), *MeshySuffix));
	J.ReplaceInline(TEXT("\"name\":\"generate_3d_model_tripo\""), *FString::Printf(TEXT("\"name\":\"generate_3d_model_tripo%s\""), *TripoSuffix));
	J.ReplaceInline(TEXT("\"name\":\"generate_voice_elevenlabs\""), *FString::Printf(TEXT("\"name\":\"generate_voice_elevenlabs%s\""), *ElevenSuffix));
	J.ReplaceInline(TEXT("\"name\":\"generate_sfx_elevenlabs\""), *FString::Printf(TEXT("\"name\":\"generate_sfx_elevenlabs%s\""), *ElevenSuffix));
	J.ReplaceInline(TEXT("\"name\":\"generate_music_elevenlabs\""), *FString::Printf(TEXT("\"name\":\"generate_music_elevenlabs%s\""), *ElevenSuffix));
	J.ReplaceInline(TEXT("\"name\":\"list_voices_elevenlabs\""), *FString::Printf(TEXT("\"name\":\"list_voices_elevenlabs%s\""), *ElevenSuffix));
	J.ReplaceInline(TEXT("\"name\":\"generate_material_fal\""), *FString::Printf(TEXT("\"name\":\"generate_material_fal%s\""), *FalSuffix));
	return J;
}

FString FNwiroToolDispatch::GetEnabledToolDefinitionsJson()
{
	return BuildToolDefinitionsJson(/*bForSettings=*/false);
}

FString FNwiroToolDispatch::GetToolDefinitionsForSettingsJson()
{
	return BuildToolDefinitionsJson(/*bForSettings=*/true);
}

// ============================================================
// Tool Dispatch — calls existing C++ tool implementations
// ============================================================

// >>> NWIRO_PARITY BEGIN: meshy-retexture-helpers
// Own-key Meshy tools: the model tool's answer, and the texture tool's retexture request and
// source-model check. Meshy retextures a finished Text to 3D preview or refine, Image to 3D or
// Remesh task, never a retexture task (docs.meshy.ai/en/api/retexture).
namespace NwiroMeshyRetexture
{
	/** Meshy's limit for text_style_prompt. */
	constexpr int32 MaxPromptChars = 800;

	struct FRequest
	{
		/** JSON body of POST /openapi/v1/retexture. */
		FString Body;
		/** The description as sent, for the task event. */
		FString Prompt;
		/** The source model's Meshy task id, empty when only a model URL was given. */
		FString InputTaskId;
	};

	/** Builds the request from the tool's arguments; false with OutError when they are invalid. */
	static bool BuildRequest(const TSharedPtr<FJsonObject>& Args, FRequest& Out, FString& OutError)
	{
		FString Prompt, ModelUrl, ModelId;
		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("prompt"), Prompt);
			Args->TryGetStringField(TEXT("model_url"), ModelUrl);
			Args->TryGetStringField(TEXT("model_id"), ModelId);
		}
		Prompt.TrimStartAndEndInline();
		ModelUrl.TrimStartAndEndInline();
		ModelId.TrimStartAndEndInline();
		if (Prompt.IsEmpty())
		{
			OutError = TEXT("prompt is required");
			return false;
		}
		if (Prompt.Len() > MaxPromptChars)
		{
			OutError = FString::Printf(TEXT("prompt must be at most %d characters"), MaxPromptChars);
			return false;
		}
		if (ModelUrl.IsEmpty() && ModelId.IsEmpty())
		{
			OutError = TEXT("Either model_url or model_id is required");
			return false;
		}

		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		// Meshy uses input_task_id when both are given.
		if (!ModelId.IsEmpty())
		{
			Body->SetStringField(TEXT("input_task_id"), ModelId);
			// Keep the UV layout of a model Meshy made. A model given by URL may have no usable
			// UVs, so it gets Meshy's default: a new layout.
			Body->SetBoolField(TEXT("enable_original_uv"), true);
		}
		if (!ModelUrl.IsEmpty()) Body->SetStringField(TEXT("model_url"), ModelUrl);
		Body->SetStringField(TEXT("text_style_prompt"), Prompt);
		Body->SetStringField(TEXT("ai_model"), TEXT("meshy-6"));
		Body->SetBoolField(TEXT("enable_pbr"), true);
		TArray<TSharedPtr<FJsonValue>> Formats;
		for (const TCHAR* Format : { TEXT("glb"), TEXT("fbx"), TEXT("obj") })
		{
			Formats.Add(MakeShared<FJsonValueString>(Format));
		}
		Body->SetArrayField(TEXT("target_formats"), Formats);

		Out.Body.Reset();
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out.Body);
		FJsonSerializer::Serialize(Body, Writer);
		Out.Prompt = Prompt;
		Out.InputTaskId = ModelId;
		return true;
	}

	enum class ESourceCheck : uint8
	{
		/** Another kind of task, or an id Meshy does not know: submit and let Meshy decide. */
		Submit,
		/** A retexture task: refuse, since Meshy cannot retexture it. */
		RefuseTexture,
		/** No reliable answer (network error, key refused, rate limit, server error, no kind): stop. */
		Fail,
	};

	/** What the answer to GET /openapi/v1/retexture/<id> means. Meshy returns any of the account's
	 *  tasks there and names its kind in `type`. Status 0 = no answer. */
	static ESourceCheck DecideSourceCheck(int32 Status, const FString& Body)
	{
		if (Status == 404) return ESourceCheck::Submit;
		if (Status < 200 || Status >= 300) return ESourceCheck::Fail;
		TSharedPtr<FJsonObject> Task;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		FString Type;
		if (!FJsonSerializer::Deserialize(Reader, Task) || !Task.IsValid()
			|| !Task->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
			return ESourceCheck::Fail;
		return Type == TEXT("retexture") ? ESourceCheck::RefuseTexture : ESourceCheck::Submit;
	}

	/** How long the model tool waits for Meshy's answer to its submit, in seconds. */
	constexpr double SubmitWaitSeconds = 5.0;

	/** The model tool's answer: submitted, with Meshy's task id when Meshy answered in time. */
	static FString ModelSubmittedResult(const FString& TaskId)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("status"), TEXT("submitted"));
		FString Message = TEXT("Generation request submitted to Meshy. This is asynchronous — the model will appear in the Meshy panel when ready. Do NOT tell the user the model is ready yet.");
		if (!TaskId.IsEmpty())
		{
			// Not "taskId": the Nwiro app reads a taskId in a tool answer as a new model, which the
			// meshy_task event already reports.
			Result->SetStringField(TEXT("meshyTaskId"), TaskId);
			Message += FString::Printf(TEXT(" Meshy task id: %s. To texture this model later, pass it as model_id to the Meshy texture tool."), *TaskId);
		}
		Result->SetStringField(TEXT("message"), Message);
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Result, Writer);
		return Out;
	}
}
// <<< NWIRO_PARITY END: meshy-retexture-helpers

namespace NwiroMeshyRetexture
{
	/** The chat a texture call's events go to when the call names none. The Nwiro chat always
	 *  names its chat; without one, the app shows the events in its active chat. */
	static FString FallbackChatId()
	{
		return FString();
	}
}

// Forward declaration: the original ~1370-branch dispatch body below, renamed
// and made TU-local. FNwiroToolDispatch::DispatchTool (still the name/signature
// the one call site, NwiroBridge.cpp:1595, uses) is a thin passthrough to this.
static FString DispatchToolImpl(const FString& ToolName, const FString& ArgsJson);

FString FNwiroToolDispatch::DispatchTool(const FString& ToolName, const FString& ArgsJson)
{
	// Reliability guard (2026-07-16): tool dispatch runs on the GAME THREAD. A malformed
	// asset name/path makes engine asset APIs raise a BLOCKING modal ("Name may not contain
	// the following characters: /") that freezes the game thread and hangs the MCP server
	// until a human clicks OK. GIsRunningUnattendedScript makes FMessageDialog::Open return
	// the dialog's default instead of blocking — the C++ analog of the execute_python
	// EPythonCommandFlags::Unattended guard. TGuardValue restores the prior value on scope
	// exit. Scope is TIGHT (the synchronous dispatch only). Audited 2026-07-16: the mutating
	// tool paths reach no destructive-default engine modal (deletes use the two-flag policy +
	// silent UEditorAssetLibrary::DeleteAsset; creates pre-check DoesAssetExist), so
	// suppression converts hang -> clean failure, never a silent mutation. Parity fix with
	// the Integration Kit plugin (NwiroIKMCPServer::DispatchTool).
	check(IsInGameThread());
	TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
	return DispatchToolImpl(ToolName, ArgsJson);
}

static FString DispatchToolImpl(const FString& ToolName, const FString& ArgsJson)
{
	// ─── Managed-tier protected rails ────────────────────────────────
	// Rails outrank every mode and every allow layer. They CANNOT be
	// bypassed by bypassPermissions or any session-allow flag. Runs at
	// the very top of dispatch so the per-tool branches below never see
	// rail-blocked calls.
#if NWIRO_PROTECTED_RAILS_ENFORCED
	{
		TSharedPtr<FJsonObject> RailsArgs;
		if (!ArgsJson.IsEmpty())
		{
			TSharedRef<TJsonReader<>> RailsReader = TJsonReaderFactory<>::Create(ArgsJson);
			FJsonSerializer::Deserialize(RailsReader, RailsArgs);
		}
		const FString RailsDeny = NwiroProtectedRails::Check(ToolName, RailsArgs);
		if (!RailsDeny.IsEmpty())
		{
			// Mirror existing tool-error string shape (see e.g. line 360
			// for the get_tool_info "Missing 'tool' parameter" pattern).
			FString Escaped = RailsDeny.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("{\"error\":\"%s\"}"), *Escaped);
		}
	}
#endif

	// get_tool_info: return detailed schema for a specific tool
	if (ToolName == TEXT("get_tool_info"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
		FJsonSerializer::Deserialize(Reader, Args);
		FString TargetTool;
		if (Args.IsValid())
		{
			if (Args->HasField(TEXT("tool"))) TargetTool = Args->GetStringField(TEXT("tool"));
			else if (Args->HasField(TEXT("tool_name"))) TargetTool = Args->GetStringField(TEXT("tool_name"));
			else if (Args->HasField(TEXT("name"))) TargetTool = Args->GetStringField(TEXT("name"));
		}
		if (TargetTool.IsEmpty()) return TEXT("{\"error\":\"Missing 'tool' parameter. Use: {\\\"tool\\\":\\\"tool_name\\\"}\"}");

		// Model-provided string — escape before interpolating into JSON errors.
		const FString SafeTool = TargetTool
			.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));

		FString AllJson = FNwiroToolDispatch::GetEnabledToolDefinitionsJson();
		TArray<TSharedPtr<FJsonValue>> AllTools;
		TSharedRef<TJsonReader<>> R2 = TJsonReaderFactory<>::Create(AllJson);
		if (FJsonSerializer::Deserialize(R2, AllTools))
		{
			for (const auto& V : AllTools)
			{
				auto Obj = V->AsObject();
				if (Obj.IsValid() && Obj->GetStringField(TEXT("name")) == TargetTool)
				{
					FString Out;
					TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
						TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
					FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
					return Out;
				}
			}
		}

		// Not in the enabled set — if it exists in the full inventory it is
		// user-disabled: say so explicitly (schema withheld, dispatch would
		// reject it anyway) rather than pretending it doesn't exist.
		FString FullJson = FNwiroToolDispatch::GetToolDefinitionsForSettingsJson();
		TArray<TSharedPtr<FJsonValue>> FullTools;
		TSharedRef<TJsonReader<>> R3 = TJsonReaderFactory<>::Create(FullJson);
		if (FJsonSerializer::Deserialize(R3, FullTools))
		{
			for (const auto& V : FullTools)
			{
				auto Obj = V->AsObject();
				if (Obj.IsValid() && Obj->GetStringField(TEXT("name")) == TargetTool)
				{
					return FString::Printf(
						TEXT("{\"error\":\"Tool '%s' exists but is disabled in Settings -> Tool Sources. Tell the user to enable its source there if they want it.\"}"),
						*SafeTool);
				}
			}
		}
		return FString::Printf(TEXT("{\"error\":\"Tool '%s' not found\"}"), *SafeTool);
	}

	// Helper: extract a string field from JSON args
	auto ExtractField = [&ArgsJson](const FString& FieldName) -> FString
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Parsed) && Parsed.IsValid() && Parsed->HasField(FieldName))
		{
			return Parsed->GetStringField(FieldName);
		}
		return ArgsJson; // fallback to raw string
	};

	// Blueprint tools
	if (ToolName == TEXT("find_blueprints"))
		return FNwiroBlueprintTools::FindBlueprints(ExtractField(TEXT("searchTerm")));
	if (ToolName == TEXT("read_blueprint"))
		return FNwiroBlueprintTools::ReadBlueprint(ArgsJson);
	if (ToolName == TEXT("edit_blueprint"))
		return FNwiroBlueprintTools::EditBlueprint(ArgsJson);
	if (ToolName == TEXT("create_blueprint"))
		return FNwiroBlueprintTools::CreateBlueprint(ArgsJson);
	if (ToolName == TEXT("render_blueprint_graph"))
		return FNwiroBlueprintTools::RenderBlueprintGraph(ArgsJson);
	if (ToolName == TEXT("organize_blueprint_nodes"))
		return FNwiroBlueprintTools::OrganizeBlueprintNodes(ArgsJson);
	if (ToolName == TEXT("find_blueprint_nodes"))
	{
		// Parse the args JSON to extract the actual query string — passing the
		// raw JSON blob as the query (the previous behaviour) meant Contains()
		// never matched anything.
		FString FbnQuery;
		FString FbnBlueprint;
		TSharedPtr<FJsonObject> FbnArgs;
		TSharedRef<TJsonReader<>> FbnReader = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(FbnReader, FbnArgs) && FbnArgs.IsValid())
		{
			FbnArgs->TryGetStringField(TEXT("query"), FbnQuery);
			FbnArgs->TryGetStringField(TEXT("blueprint"), FbnBlueprint);
		}
		return FNwiroBlueprintTools::FindBlueprintNodes(FbnQuery, FbnBlueprint);
	}
	if (ToolName == TEXT("clear_graph"))
		return FNwiroBlueprintTools::ClearGraph(ArgsJson);
	if (ToolName == TEXT("delete_blueprint"))
		return FNwiroBlueprintTools::DeleteBlueprint(ArgsJson);
	if (ToolName == TEXT("duplicate_blueprint"))
		return FNwiroBlueprintTools::DuplicateBlueprint(ArgsJson);
	if (ToolName == TEXT("rename_blueprint"))
		return FNwiroBlueprintTools::RenameBlueprint(ArgsJson);
	if (ToolName == TEXT("delete_node"))
		return FNwiroBlueprintTools::DeleteNode(ArgsJson);
	if (ToolName == TEXT("create_function_graph"))
		return FNwiroBlueprintTools::CreateFunctionGraph(ArgsJson);
	if (ToolName == TEXT("add_interface"))
		return FNwiroBlueprintTools::AddInterface(ArgsJson);
	if (ToolName == TEXT("remove_interface"))
		return FNwiroBlueprintTools::RemoveInterface(ArgsJson);
	if (ToolName == TEXT("create_event_dispatcher"))
		return FNwiroBlueprintTools::CreateEventDispatcher(ArgsJson);
	if (ToolName == TEXT("reparent_blueprint"))
		return FNwiroBlueprintTools::ReparentBlueprint(ArgsJson);
	if (ToolName == TEXT("remove_component"))
		return FNwiroBlueprintTools::RemoveComponent(ArgsJson);
	if (ToolName == TEXT("edit_component"))
		return FNwiroBlueprintTools::EditComponent(ArgsJson);

	// Material tools
	if (ToolName == TEXT("find_materials"))
		return FNwiroMaterialTools::FindMaterials(ArgsJson);
	if (ToolName == TEXT("inspect_material"))
		return FNwiroMaterialTools::InspectMaterial(ArgsJson);
	if (ToolName == TEXT("create_material"))
		return FNwiroMaterialTools::CreateMaterial(ArgsJson);
	if (ToolName == TEXT("edit_material"))
		return FNwiroMaterialTools::EditMaterial(ArgsJson);
	if (ToolName == TEXT("inspect_material_graph"))
		return FNwiroMaterialTools::InspectMaterialGraph(ArgsJson);
	if (ToolName == TEXT("create_material_instance"))
		return FNwiroMaterialTools::CreateMaterialInstance(ArgsJson);
	if (ToolName == TEXT("find_textures"))
		return FNwiroMaterialTools::FindTextures(ArgsJson);
	if (ToolName == TEXT("apply_material"))
		return FNwiroMaterialTools::ApplyMaterial(ArgsJson);
	if (ToolName == TEXT("delete_material"))
		return FNwiroMaterialTools::DeleteMaterial(ArgsJson);
	if (ToolName == TEXT("set_material_property"))
		return FNwiroMaterialTools::SetMaterialProperty(ArgsJson);
	if (ToolName == TEXT("edit_material_instance"))
		return FNwiroMaterialTools::EditMaterialInstance(ArgsJson);
	if (ToolName == TEXT("delete_expression"))
		return FNwiroMaterialTools::DeleteExpression(ArgsJson);
	if (ToolName == TEXT("find_material_functions"))
		return FNwiroMaterialTools::FindMaterialFunctions(ArgsJson);
	if (ToolName == TEXT("create_material_function"))
		return FNwiroMaterialTools::CreateMaterialFunction(ArgsJson);
	if (ToolName == TEXT("inspect_material_function"))
		return FNwiroMaterialTools::InspectMaterialFunction(ArgsJson);
	if (ToolName == TEXT("edit_material_function"))
		return FNwiroMaterialTools::EditMaterialFunction(ArgsJson);
	if (ToolName == TEXT("delete_material_function"))
		return FNwiroMaterialTools::DeleteMaterialFunction(ArgsJson);

	// Settings tools
	if (ToolName == TEXT("get_world_settings"))
		return FNwiroSettingsTools::GetWorldSettings();
	if (ToolName == TEXT("set_world_settings"))
		return FNwiroSettingsTools::SetWorldSettings(ArgsJson);
	if (ToolName == TEXT("set_game_mode"))
		return FNwiroSettingsTools::SetGameMode(ArgsJson);
	if (ToolName == TEXT("get_project_settings"))
		return FNwiroSettingsTools::GetProjectSettings(ArgsJson);
	if (ToolName == TEXT("set_project_settings"))
		return FNwiroSettingsTools::SetProjectSettings(ArgsJson);
	if (ToolName == TEXT("get_level_actors"))
		return FNwiroSettingsTools::GetLevelActors(ArgsJson);
	if (ToolName == TEXT("spawn_actor"))
		return FNwiroSettingsTools::SpawnActor(ArgsJson);
	if (ToolName == TEXT("delete_actor"))
		return FNwiroSettingsTools::DeleteActor(ArgsJson);
	if (ToolName == TEXT("transform_actor"))
		return FNwiroSettingsTools::TransformActor(ArgsJson);
	if (ToolName == TEXT("get_actor_property"))
		return FNwiroSettingsTools::GetActorProperty(ArgsJson);
	if (ToolName == TEXT("set_actor_property"))
		return FNwiroSettingsTools::SetActorProperty(ArgsJson);
	if (ToolName == TEXT("execute_python"))
		return FNwiroSettingsTools::ExecutePython(ArgsJson);
	if (ToolName == TEXT("search_assets"))
	{
		if (UNwiroBridge::Instance) return UNwiroBridge::Instance->SearchAssets(ArgsJson);
		// Inline fallback when Bridge isn't ready yet (very early startup).
		FString SearchTerm;
		TSharedPtr<FJsonObject> JsonObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
			JsonObj->TryGetStringField(TEXT("query"), SearchTerm);
		const bool bShowAll = SearchTerm.IsEmpty() || SearchTerm == TEXT("*");
		const FString QueryLower = SearchTerm.ToLower();
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		// `false` second arg = include in-memory assets too. Without this,
		// BPs the agent just created via create_blueprint but didn't save
		// yet are invisible — and most LLM sessions never save between turns.
		TArray<FAssetData> AllAssets;
		ARM.Get().GetAllAssets(AllAssets, false);
		FString Out = TEXT("{\"success\":true,\"assets\":[");
		int32 Count = 0;
		bool bFirst = true;
		for (const FAssetData& A : AllAssets)
		{
			if (Count >= 15) break;
			const FString Path = A.GetObjectPathString();
			if (!Path.StartsWith(TEXT("/Game/"))) continue;
			const FString Name = A.AssetName.ToString();
			if (!bShowAll && !Name.ToLower().Contains(QueryLower)) continue;
			if (!bFirst) Out += TEXT(",");
			Out += FString::Printf(TEXT("{\"name\":\"%s\",\"path\":\"%s\",\"class\":\"%s\"}"),
				*Name, *Path.Replace(TEXT("\""), TEXT("\\\"")), *A.AssetClassPath.GetAssetName().ToString());
			bFirst = false;
			Count++;
		}
		Out += FString::Printf(TEXT("],\"count\":%d,\"source\":\"inline-fallback\"}"), Count);
		return Out;
	}

	// PCG (default tools — Extended PCG extension uses its own server pipeline)
	if (ToolName == TEXT("create_pcg_graph"))
		return FNwiroPCGTools::CreatePcgGraph(ArgsJson);
	if (ToolName == TEXT("find_pcg_graphs"))
		return FNwiroPCGTools::FindPcgGraphs(ArgsJson);
	if (ToolName == TEXT("spawn_pcg_volume"))
		return FNwiroPCGTools::SpawnPcgVolume(ArgsJson);
	if (ToolName == TEXT("pcg_generate"))
		return FNwiroPCGTools::PcgGenerate(ArgsJson);
	if (ToolName == TEXT("add_pcg_node"))
		return FNwiroPCGTools::AddPcgNode(ArgsJson);

	// File operations — require File Editor extension
	if (ToolName == TEXT("write_file") || ToolName == TEXT("read_file") || ToolName == TEXT("delete_file") || ToolName == TEXT("rename_file"))
	{
		if (UNwiroBridge::Instance && !UNwiroBridge::Instance->IsChatExtensionEnabled(TEXT("fileEditor")))
			return TEXT("{\"success\":false,\"error\":\"File Editor extension is not enabled for this chat. Enable it from the Extensions menu in the chat input area.\"}");
	}

	// Hallucination-tolerant arg picker — returns first non-empty string in Names.
	auto PickFileArg = [](const TSharedPtr<FJsonObject>& Args, std::initializer_list<const TCHAR*> Names) -> FString
	{
		for (const TCHAR* N : Names)
		{
			FString V;
			if (Args->TryGetStringField(N, V) && !V.IsEmpty())
				return V;
		}
		return FString();
	};

	if (ToolName == TEXT("write_file"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			FString FilePath = PickFileArg(Args, { TEXT("file_path"), TEXT("path"), TEXT("filePath"), TEXT("filepath"), TEXT("filename"), TEXT("file") });
			FString Content = PickFileArg(Args, { TEXT("content"), TEXT("text"), TEXT("body"), TEXT("data") });
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"file_path required (accepted: file_path/path/filePath/filename)\"}");
			// Sandbox path against ProjectDir — rejects /etc/passwd, ../escape, etc.
			FilePath = NwiroPathSandbox::TryResolveSandboxed(FilePath);
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Path outside project sandbox.\"}");
			bool bOk = FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return FString::Printf(TEXT("{\"success\":%s,\"file_path\":\"%s\",\"content\":%s}"),
				bOk ? TEXT("true") : TEXT("false"), *FilePath.Replace(TEXT("\\"), TEXT("/")),
				bOk ? TEXT("\"written\"") : TEXT("\"failed\""));
		}
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}
	if (ToolName == TEXT("read_file"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			FString FilePath = PickFileArg(Args, { TEXT("file_path"), TEXT("path"), TEXT("filePath"), TEXT("filepath"), TEXT("filename"), TEXT("file") });
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"file_path required (accepted: file_path/path/filePath/filename)\"}");
			FilePath = NwiroPathSandbox::TryResolveSandboxed(FilePath);
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Path outside project sandbox.\"}");
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *FilePath))
			{
				Content.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
				Content.ReplaceInline(TEXT("\""), TEXT("\\\""));
				Content.ReplaceInline(TEXT("\n"), TEXT("\\n"));
				Content.ReplaceInline(TEXT("\r"), TEXT(""));
				Content.ReplaceInline(TEXT("\t"), TEXT("\\t"));
				return FString::Printf(TEXT("{\"success\":true,\"file_path\":\"%s\",\"content\":\"%s\"}"),
					*FilePath.Replace(TEXT("\\"), TEXT("/")), *Content);
			}
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"File not found: %s\"}"), *FilePath);
		}
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}
	if (ToolName == TEXT("delete_file"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			FString FilePath = PickFileArg(Args, { TEXT("file_path"), TEXT("path"), TEXT("filePath"), TEXT("filepath"), TEXT("filename"), TEXT("file") });
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"file_path required (accepted: file_path/path/filePath/filename)\"}");
			FilePath = NwiroPathSandbox::TryResolveSandboxed(FilePath);
			if (FilePath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Path outside project sandbox.\"}");
			bool bOk = IFileManager::Get().Delete(*FilePath);
			return FString::Printf(TEXT("{\"success\":%s,\"file_path\":\"%s\"}"),
				bOk ? TEXT("true") : TEXT("false"), *FilePath.Replace(TEXT("\\"), TEXT("/")));
		}
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}
	if (ToolName == TEXT("rename_file"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			FString OldPath = PickFileArg(Args, { TEXT("old_path"), TEXT("from"), TEXT("source"), TEXT("src"), TEXT("oldPath"), TEXT("old"), TEXT("old_file"), TEXT("oldFile") });
			FString NewPath = PickFileArg(Args, { TEXT("new_path"), TEXT("to"), TEXT("dest"), TEXT("destination"), TEXT("newPath"), TEXT("new"), TEXT("new_file"), TEXT("newFile") });
			if (OldPath.IsEmpty() || NewPath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"old_path and new_path required (accepted: old_path/from/source, new_path/to/dest)\"}");
			OldPath = NwiroPathSandbox::TryResolveSandboxed(OldPath);
			NewPath = NwiroPathSandbox::TryResolveSandboxed(NewPath);
			if (OldPath.IsEmpty() || NewPath.IsEmpty())
				return TEXT("{\"success\":false,\"error\":\"Path outside project sandbox.\"}");
			bool bOk = IFileManager::Get().Move(*NewPath, *OldPath);
			return FString::Printf(TEXT("{\"success\":%s,\"old_path\":\"%s\",\"new_path\":\"%s\"}"),
				bOk ? TEXT("true") : TEXT("false"),
				*OldPath.Replace(TEXT("\\"), TEXT("/")), *NewPath.Replace(TEXT("\\"), TEXT("/")));
		}
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}

	// SCG artifact I/O, sandboxed to the SCG run root. NOT ADVERTISED to the
	// chat model — the panel calls these via bridge.executetool, which does not
	// consult the advertised list. Contracts in NwiroScgTools.h.
	if (ToolName == TEXT("scg_write_artifact"))
		return FNwiroScgTools::WriteArtifact(ArgsJson);
	if (ToolName == TEXT("scg_read_artifact"))
		return FNwiroScgTools::ReadArtifact(ArgsJson);
	if (ToolName == TEXT("scg_list_runs"))
		return FNwiroScgTools::ListRuns(ArgsJson);
	if (ToolName == TEXT("scg_read_artifacts"))
		return FNwiroScgTools::ReadArtifacts(ArgsJson);
	if (ToolName == TEXT("scg_artifacts_exist"))
		return FNwiroScgTools::ArtifactsExist(ArgsJson);

	if (ToolName == TEXT("duplicate_actor"))
		return FNwiroSettingsTools::DuplicateActor(ArgsJson);
	if (ToolName == TEXT("rename_actor"))
		return FNwiroSettingsTools::RenameActor(ArgsJson);
	if (ToolName == TEXT("attach_actor"))
		return FNwiroSettingsTools::AttachActor(ArgsJson);
	if (ToolName == TEXT("detach_actor"))
		return FNwiroSettingsTools::DetachActor(ArgsJson);
	if (ToolName == TEXT("select_actor"))
		return FNwiroSettingsTools::SelectActor(ArgsJson);

	// Input tools
	if (ToolName == TEXT("create_input_action"))
		return FNwiroInputTools::CreateInputAction(ArgsJson);
	if (ToolName == TEXT("create_input_mapping_context"))
		return FNwiroInputTools::CreateInputMappingContext(ArgsJson);
	if (ToolName == TEXT("find_input_actions"))
		return FNwiroInputTools::FindInputActions(ArgsJson);
	if (ToolName == TEXT("delete_input_action"))
		return FNwiroInputTools::DeleteInputAction(ArgsJson);
	if (ToolName == TEXT("edit_mapping_context"))
		return FNwiroInputTools::EditMappingContext(ArgsJson);

	// Asset tools
	if (ToolName == TEXT("read_asset"))
		return FNwiroAssetTools::ReadAsset(ArgsJson);
	if (ToolName == TEXT("save_asset"))
		return FNwiroAssetTools::SaveAsset(ArgsJson);
	if (ToolName == TEXT("find_assets"))
		return FNwiroAssetTools::FindAssets(ArgsJson);
	if (ToolName == TEXT("find_static_meshes"))
	{
		// Alias: inject classFilter="StaticMesh" into args and delegate to
		// FindAssets. Keeps the advertised tool cheap (no new impl) while
		// preserving the searchTerm + path + maxResults path FindAssets
		// already handles. Empty searchTerm = discovery mode (list all
		// meshes in path), matching find_input_actions precedent.
		TSharedPtr<FJsonObject> Cmd;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Cmd) || !Cmd.IsValid())
			Cmd = MakeShareable(new FJsonObject());
		Cmd->SetStringField(TEXT("classFilter"), TEXT("StaticMesh"));
		FString Rewritten;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Rewritten);
		FJsonSerializer::Serialize(Cmd.ToSharedRef(), W);
		return FNwiroAssetTools::FindAssets(Rewritten);
	}
	if (ToolName == TEXT("get_asset_thumbnail"))
		return FNwiroAssetTools::GetAssetThumbnail(ArgsJson);

	// Editor tools
	if (ToolName == TEXT("take_screenshot"))
		return FNwiroEditorTools::TakeScreenshot(ArgsJson);
	if (ToolName == TEXT("read_log"))
		return FNwiroEditorTools::ReadLog(ArgsJson);
	if (ToolName == TEXT("play_in_editor"))
		return FNwiroEditorTools::PlayInEditor(ArgsJson);
	if (ToolName == TEXT("stop_pie"))
		return FNwiroEditorTools::StopPIE(ArgsJson);

	// Render tools
	if (ToolName == TEXT("render_still_passes"))
		return FNwiroRenderTools::RenderStillPasses(ArgsJson);
	if (ToolName == TEXT("render_pass_sequence"))
		return FNwiroRenderTools::RenderPassSequence(ArgsJson);
	if (ToolName == TEXT("get_render_job_status"))
		return FNwiroRenderTools::GetRenderJobStatus(ArgsJson);
	if (ToolName == TEXT("cancel_render_job"))
		return FNwiroRenderTools::CancelRenderJob(ArgsJson);
	if (ToolName == TEXT("ensure_provisioned"))
		return FNwiroRenderTools::EnsureProvisioned(ArgsJson);
	if (ToolName == TEXT("scg_open_artifact"))
		return FNwiroScgTools::OpenArtifact(ArgsJson);
	// Panel-only, like the artifact I/O tools above: the CEF page cannot open a
	// file dialog (UE's SWebBrowser refuses OnFileDialog outright), so the
	// canvas's "choose a file" reaches the OS through here. NOT advertised to
	// the chat model — a file dialog needs a human at the keyboard.
	if (ToolName == TEXT("scg_pick_file"))
		return FNwiroScgTools::PickFile(ArgsJson);
	// Also panel-only, and for the same reason: both exist so the canvas can tell
	// a user where a generated file lives, which a hosted page cannot know.
	if (ToolName == TEXT("scg_reveal_artifact"))
		return FNwiroScgTools::RevealArtifact(ArgsJson);
	if (ToolName == TEXT("scg_runs_root"))
		return FNwiroScgTools::RunsRoot(ArgsJson);

	// Data tools
	if (ToolName == TEXT("create_data_table"))
		return FNwiroDataTools::CreateDataTable(ArgsJson);
	if (ToolName == TEXT("add_data_table_row"))
		return FNwiroDataTools::AddDataTableRow(ArgsJson);
	if (ToolName == TEXT("read_data_table"))
		return FNwiroDataTools::ReadDataTable(ArgsJson);
	if (ToolName == TEXT("import_data_table_json"))
		return FNwiroDataTools::ImportDataTableJson(ArgsJson);
	if (ToolName == TEXT("create_struct"))
		return FNwiroDataTools::CreateStruct(ArgsJson);
	if (ToolName == TEXT("create_enum"))
		return FNwiroDataTools::CreateEnum(ArgsJson);

	// Animation tools
	if (ToolName == TEXT("create_montage"))
		return FNwiroAnimTools::CreateMontage(ArgsJson);
	if (ToolName == TEXT("read_montage"))
		return FNwiroAnimTools::ReadMontage(ArgsJson);
	if (ToolName == TEXT("add_montage_section"))
		return FNwiroAnimTools::AddMontageSection(ArgsJson);
	if (ToolName == TEXT("link_montage_sections"))
		return FNwiroAnimTools::LinkMontageSections(ArgsJson);
	if (ToolName == TEXT("add_montage_notify"))
		return FNwiroAnimTools::AddMontageNotify(ArgsJson);
	if (ToolName == TEXT("create_anim_blueprint"))
		return FNwiroAnimTools::CreateAnimBlueprint(ArgsJson);
	if (ToolName == TEXT("read_anim_blueprint"))
		return FNwiroAnimTools::ReadAnimBlueprint(ArgsJson);

	// Sequencer tools
	if (ToolName == TEXT("create_sequence"))
		return FNwiroSequencerTools::CreateSequence(ArgsJson);
	if (ToolName == TEXT("read_sequence"))
		return FNwiroSequencerTools::ReadSequence(ArgsJson);
	if (ToolName == TEXT("add_sequence_binding"))
		return FNwiroSequencerTools::AddSequenceBinding(ArgsJson);
	if (ToolName == TEXT("add_sequence_track"))
		return FNwiroSequencerTools::AddSequenceTrack(ArgsJson);
	if (ToolName == TEXT("add_sequence_keyframe"))
		return FNwiroSequencerTools::AddSequenceKeyframe(ArgsJson);
	if (ToolName == TEXT("set_sequence_range"))
		return FNwiroSequencerTools::SetSequenceRange(ArgsJson);

	// AI tools
	if (ToolName == TEXT("create_behavior_tree"))
		return FNwiroAITools::CreateBehaviorTree(ArgsJson);
	if (ToolName == TEXT("read_behavior_tree"))
		return FNwiroAITools::ReadBehaviorTree(ArgsJson);
	if (ToolName == TEXT("create_blackboard"))
		return FNwiroAITools::CreateBlackboard(ArgsJson);
	if (ToolName == TEXT("edit_blackboard"))
		return FNwiroAITools::EditBlackboard(ArgsJson);

	// Widget tools
	if (ToolName == TEXT("create_widget_blueprint"))
		return FNwiroWidgetTools::CreateWidgetBlueprint(ArgsJson);
	if (ToolName == TEXT("observe_blueprint_state"))
		return FNwiroRequestedStateTools::ObserveBlueprintState(ArgsJson);
	if (ToolName == TEXT("observe_input_action_state"))
		return FNwiroRequestedStateTools::ObserveInputActionState(ArgsJson);
	if (ToolName == TEXT("read_widget_blueprint"))
		return FNwiroWidgetTools::ReadWidgetBlueprint(ArgsJson);
	if (ToolName == TEXT("add_widget"))
		return FNwiroWidgetTools::AddWidget(ArgsJson);
	if (ToolName == TEXT("set_widget_property"))
		return FNwiroWidgetTools::SetWidgetProperty(ArgsJson);
	if (ToolName == TEXT("render_widget_blueprint"))
		return FNwiroWidgetTools::RenderWidgetBlueprint(ArgsJson);

	// Niagara tools
	if (ToolName == TEXT("create_niagara_system"))
		return FNwiroNiagaraTools::CreateNiagaraSystem(ArgsJson);
	if (ToolName == TEXT("read_niagara_system"))
		return FNwiroNiagaraTools::ReadNiagaraSystem(ArgsJson);
	if (ToolName == TEXT("set_niagara_parameter"))
		return FNwiroNiagaraTools::SetNiagaraParameter(ArgsJson);

	// State Tree tools
	if (ToolName == TEXT("create_state_tree"))
		return FNwiroStateTreeTools::CreateStateTree(ArgsJson);
	if (ToolName == TEXT("read_state_tree"))
		return FNwiroStateTreeTools::ReadStateTree(ArgsJson);
	if (ToolName == TEXT("add_state_tree_state"))
		return FNwiroStateTreeTools::AddStateTreeState(ArgsJson);

#if NWIRO_HAS_IK_TOOLS
	// IK Rig tools
	if (ToolName == TEXT("create_ik_rig"))
		return FNwiroIKTools::CreateIKRig(ArgsJson);
	if (ToolName == TEXT("read_ik_rig"))
		return FNwiroIKTools::ReadIKRig(ArgsJson);
	if (ToolName == TEXT("add_ik_goal"))
		return FNwiroIKTools::AddIKGoal(ArgsJson);
	if (ToolName == TEXT("add_ik_solver"))
		return FNwiroIKTools::AddIKSolver(ArgsJson);
	if (ToolName == TEXT("add_retarget_chain"))
		return FNwiroIKTools::AddRetargetChain(ArgsJson);

	// IK Retargeter tools
	if (ToolName == TEXT("create_ik_retargeter"))
		return FNwiroIKTools::CreateIKRetargeter(ArgsJson);
	if (ToolName == TEXT("read_ik_retargeter"))
		return FNwiroIKTools::ReadIKRetargeter(ArgsJson);
	if (ToolName == TEXT("set_chain_mapping"))
		return FNwiroIKTools::SetChainMapping(ArgsJson);

	// Pose Search / Motion Matching tools
	if (ToolName == TEXT("create_pose_search_schema"))
		return FNwiroIKTools::CreatePoseSearchSchema(ArgsJson);
	if (ToolName == TEXT("create_pose_search_database"))
		return FNwiroIKTools::CreatePoseSearchDatabase(ArgsJson);
	if (ToolName == TEXT("read_pose_search_database"))
		return FNwiroIKTools::ReadPoseSearchDatabase(ArgsJson);
	if (ToolName == TEXT("add_pose_search_animation"))
		return FNwiroIKTools::AddPoseSearchAnimation(ArgsJson);
#endif // NWIRO_HAS_IK_TOOLS

	// BT / AnimBP / Niagara edit tools
	if (ToolName == TEXT("add_behavior_tree_nodes"))
		return FNwiroAITools::AddBehaviorTreeNodes(ArgsJson);
	if (ToolName == TEXT("add_anim_bp_state_machine"))
		return FNwiroAnimTools::AddAnimBPStateMachine(ArgsJson);
	if (ToolName == TEXT("add_niagara_emitter"))
		return FNwiroNiagaraTools::AddNiagaraEmitter(ArgsJson);

	// Environment tools
	if (ToolName == TEXT("set_post_process"))
		return FNwiroEnvironmentTools::SetPostProcess(ArgsJson);
	if (ToolName == TEXT("set_fog"))
		return FNwiroEnvironmentTools::SetFog(ArgsJson);
	if (ToolName == TEXT("set_sky_atmosphere"))
		return FNwiroEnvironmentTools::SetSkyAtmosphere(ArgsJson);
	if (ToolName == TEXT("set_light_properties"))
		return FNwiroEnvironmentTools::SetLightProperties(ArgsJson);

	// Physics tools
	if (ToolName == TEXT("set_physics_simulation"))
		return FNwiroEnvironmentTools::SetPhysicsSimulation(ArgsJson);
	if (ToolName == TEXT("set_collision_profile"))
		return FNwiroEnvironmentTools::SetCollisionProfile(ArgsJson);
	if (ToolName == TEXT("add_physics_constraint"))
		return FNwiroEnvironmentTools::AddPhysicsConstraint(ArgsJson);
	if (ToolName == TEXT("get_physics_info"))
		return FNwiroEnvironmentTools::GetPhysicsInfo(ArgsJson);

	// Spline tools
	if (ToolName == TEXT("create_spline_actor"))
		return FNwiroEnvironmentTools::CreateSplineActor(ArgsJson);
	if (ToolName == TEXT("add_spline_point"))
		return FNwiroEnvironmentTools::AddSplinePoint(ArgsJson);
	if (ToolName == TEXT("set_spline_point"))
		return FNwiroEnvironmentTools::SetSplinePoint(ArgsJson);
	if (ToolName == TEXT("remove_spline_point"))
		return FNwiroEnvironmentTools::RemoveSplinePoint(ArgsJson);
	if (ToolName == TEXT("get_spline_info"))
		return FNwiroEnvironmentTools::GetSplineInfo(ArgsJson);
	if (ToolName == TEXT("set_spline_closed"))
		return FNwiroEnvironmentTools::SetSplineClosed(ArgsJson);
	if (ToolName == TEXT("set_spline_point_type"))
		return FNwiroEnvironmentTools::SetSplinePointType(ArgsJson);

	// Navigation tools
	if (ToolName == TEXT("build_navigation"))
		return FNwiroGameplayTools::BuildNavigation(ArgsJson);
	if (ToolName == TEXT("query_navigation_path"))
		return FNwiroGameplayTools::QueryNavigationPath(ArgsJson);
	if (ToolName == TEXT("get_navigation_info"))
		return FNwiroGameplayTools::GetNavigationInfo(ArgsJson);

	// Audio tools
	if (ToolName == TEXT("spawn_sound"))
		return FNwiroGameplayTools::SpawnSound(ArgsJson);
	if (ToolName == TEXT("set_audio_properties"))
		return FNwiroGameplayTools::SetAudioProperties(ArgsJson);
	if (ToolName == TEXT("get_sound_info"))
		return FNwiroGameplayTools::GetSoundInfo(ArgsJson);

	// Game Framework tools
	if (ToolName == TEXT("create_game_mode"))
		return FNwiroGameplayTools::CreateGameMode(ArgsJson);
	if (ToolName == TEXT("create_player_controller"))
		return FNwiroGameplayTools::CreatePlayerController(ArgsJson);
	if (ToolName == TEXT("create_game_state"))
		return FNwiroGameplayTools::CreateGameState(ArgsJson);
	if (ToolName == TEXT("create_player_state"))
		return FNwiroGameplayTools::CreatePlayerState(ArgsJson);
	if (ToolName == TEXT("create_hud"))
		return FNwiroGameplayTools::CreateHUD(ArgsJson);
	if (ToolName == TEXT("get_game_framework_info"))
		return FNwiroGameplayTools::GetGameFrameworkInfo(ArgsJson);

	// Build / Validation tools
	if (ToolName == TEXT("get_project_info"))
		return FNwiroGameplayTools::GetProjectInfo(ArgsJson);
	if (ToolName == TEXT("list_project_modules"))
		return FNwiroGameplayTools::ListProjectModules(ArgsJson);
	if (ToolName == TEXT("validate_assets"))
		return FNwiroGameplayTools::ValidateAssets(ArgsJson);
	if (ToolName == TEXT("get_map_check_errors"))
		return FNwiroGameplayTools::GetMapCheckErrors(ArgsJson);
	if (ToolName == TEXT("get_build_configuration"))
		return FNwiroGameplayTools::GetBuildConfiguration(ArgsJson);

	// Resources
	if (ToolName == TEXT("list_resources"))
		return FNwiroResourceProvider::GetResourceList();
	if (ToolName == TEXT("read_resource"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> RR = TJsonReaderFactory<>::Create(ArgsJson);
		if (FJsonSerializer::Deserialize(RR, Args) && Args.IsValid())
			return FNwiroResourceProvider::ReadResource(Args->GetStringField(TEXT("uri")));
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");
	}

	// Level tools
	if (ToolName == TEXT("new_level")) return FNwiroLevelTools::NewLevel(ArgsJson);
	if (ToolName == TEXT("open_level")) return FNwiroLevelTools::OpenLevel(ArgsJson);
	if (ToolName == TEXT("save_level")) return FNwiroLevelTools::SaveLevel(ArgsJson);
	if (ToolName == TEXT("get_level_info")) return FNwiroLevelTools::GetLevelInfo(ArgsJson);

	// Macro tools
	if (ToolName == TEXT("create_basic_level")) return FNwiroLevelTools::CreateBasicLevel(ArgsJson);
	if (ToolName == TEXT("create_light_rig")) return FNwiroLevelTools::CreateLightRig(ArgsJson);
	if (ToolName == TEXT("create_grid_layout")) return FNwiroLevelTools::CreateGridLayout(ArgsJson);
	if (ToolName == TEXT("create_ring_layout")) return FNwiroLevelTools::CreateRingLayout(ArgsJson);

	// Landscape tools
	if (ToolName == TEXT("create_landscape")) return FNwiroLevelTools::CreateLandscape(ArgsJson);
	if (ToolName == TEXT("set_landscape_material")) return FNwiroLevelTools::SetLandscapeMaterial(ArgsJson);
	if (ToolName == TEXT("get_landscape_info")) return FNwiroLevelTools::GetLandscapeInfo(ArgsJson);

	// Foliage tools
	if (ToolName == TEXT("add_foliage_type")) return FNwiroLevelTools::AddFoliageType(ArgsJson);
	if (ToolName == TEXT("paint_foliage")) return FNwiroLevelTools::PaintFoliage(ArgsJson);
	if (ToolName == TEXT("erase_foliage")) return FNwiroLevelTools::EraseFoliage(ArgsJson);
	if (ToolName == TEXT("get_foliage_stats")) return FNwiroLevelTools::GetFoliageStats(ArgsJson);

	// Networking tools
	if (ToolName == TEXT("get_replication_info")) return FNwiroLevelTools::GetReplicationInfo(ArgsJson);
	if (ToolName == TEXT("set_replication_settings")) return FNwiroLevelTools::SetReplicationSettings(ArgsJson);
	if (ToolName == TEXT("set_net_dormancy")) return FNwiroLevelTools::SetNetDormancy(ArgsJson);

	// World Partition tools
	if (ToolName == TEXT("get_world_partition_info")) return FNwiroLevelTools::GetWorldPartitionInfo(ArgsJson);
	if (ToolName == TEXT("load_world_partition_region")) return FNwiroLevelTools::LoadWorldPartitionRegion(ArgsJson);

	// Undo/Redo
	if (ToolName == TEXT("undo")) return FNwiroLevelTools::Undo(ArgsJson);
	if (ToolName == TEXT("redo")) return FNwiroLevelTools::Redo(ArgsJson);

	// GAS tools
	if (ToolName == TEXT("create_gameplay_ability")) return FNwiroGASTools::CreateGameplayAbility(ArgsJson);
	if (ToolName == TEXT("create_gameplay_effect")) return FNwiroGASTools::CreateGameplayEffect(ArgsJson);
	if (ToolName == TEXT("create_attribute_set")) return FNwiroGASTools::CreateAttributeSet(ArgsJson);
	if (ToolName == TEXT("list_gameplay_abilities")) return FNwiroGASTools::ListGameplayAbilities(ArgsJson);
	if (ToolName == TEXT("list_gameplay_effects")) return FNwiroGASTools::ListGameplayEffects(ArgsJson);
	if (ToolName == TEXT("list_attribute_sets")) return FNwiroGASTools::ListAttributeSets(ArgsJson);
	if (ToolName == TEXT("get_gas_info")) return FNwiroGASTools::GetGASInfo(ArgsJson);

	// PCG tools — handled by FNwiroPCGTools higher up in the dispatch.

	// PIE Runtime Control
	if (ToolName == TEXT("pie_teleport_actor")) return FNwiroPIETools::PIETeleportActor(ArgsJson);
	if (ToolName == TEXT("pie_spawn_actor")) return FNwiroPIETools::PIESpawnActor(ArgsJson);
	if (ToolName == TEXT("pie_destroy_actor")) return FNwiroPIETools::PIEDestroyActor(ArgsJson);
	if (ToolName == TEXT("pie_get_property")) return FNwiroPIETools::PIEGetProperty(ArgsJson);
	if (ToolName == TEXT("pie_set_property")) return FNwiroPIETools::PIESetProperty(ArgsJson);
	if (ToolName == TEXT("pie_set_blackboard_key")) return FNwiroPIETools::PIESetBlackboardKey(ArgsJson);
	if (ToolName == TEXT("pie_get_blackboard_key")) return FNwiroPIETools::PIEGetBlackboardKey(ArgsJson);
	if (ToolName == TEXT("pie_move_ai_to")) return FNwiroPIETools::PIEMoveAITo(ArgsJson);
	if (ToolName == TEXT("pie_stop_ai")) return FNwiroPIETools::PIEStopAI(ArgsJson);
	if (ToolName == TEXT("pie_get_game_state")) return FNwiroPIETools::PIEGetGameState(ArgsJson);
	if (ToolName == TEXT("pie_list_actors")) return FNwiroPIETools::PIEListActors(ArgsJson);
	if (ToolName == TEXT("pie_console_command")) return FNwiroPIETools::PIEConsoleCommand(ArgsJson);

	// Blueprint Debugger
	if (ToolName == TEXT("bp_get_compile_errors")) return FNwiroDebugTools::BPGetCompileErrors(ArgsJson);
	if (ToolName == TEXT("bp_set_breakpoint")) return FNwiroDebugTools::BPSetBreakpoint(ArgsJson);
	if (ToolName == TEXT("bp_remove_breakpoint")) return FNwiroDebugTools::BPRemoveBreakpoint(ArgsJson);
	if (ToolName == TEXT("bp_list_breakpoints")) return FNwiroDebugTools::BPListBreakpoints(ArgsJson);
	if (ToolName == TEXT("bp_get_watch_values")) return FNwiroDebugTools::BPGetWatchValues(ArgsJson);
	if (ToolName == TEXT("bp_add_watch")) return FNwiroDebugTools::BPAddWatch(ArgsJson);

	// Blueprint Error Fixer
	if (ToolName == TEXT("bp_fix_broken_references")) return FNwiroDebugTools::BPFixBrokenReferences(ArgsJson);
	if (ToolName == TEXT("bp_fix_deprecated_nodes")) return FNwiroDebugTools::BPFixDeprecatedNodes(ArgsJson);
	if (ToolName == TEXT("bp_refresh_all_nodes")) return FNwiroDebugTools::BPRefreshAllNodes(ArgsJson);
	if (ToolName == TEXT("bp_find_unconnected_pins")) return FNwiroDebugTools::BPFindUnconnectedPins(ArgsJson);

	// Asset Dependency
	if (ToolName == TEXT("get_asset_references")) return FNwiroDebugTools::GetAssetReferences(ArgsJson);
	if (ToolName == TEXT("get_asset_referencers")) return FNwiroDebugTools::GetAssetReferencers(ArgsJson);
	if (ToolName == TEXT("find_orphan_assets")) return FNwiroDebugTools::FindOrphanAssets(ArgsJson);
	if (ToolName == TEXT("find_circular_dependencies")) return FNwiroDebugTools::FindCircularDependencies(ArgsJson);
	if (ToolName == TEXT("get_dependency_tree")) return FNwiroDebugTools::GetDependencyTree(ArgsJson);

	// SetCDOProperty
	if (ToolName == TEXT("set_cdo_property"))
	{
		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

		FString BPName = Args->GetStringField(TEXT("blueprint"));
		FString PropName = Args->GetStringField(TEXT("property"));
		FString Value = Args->GetStringField(TEXT("value"));

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BPName);
		if (!BP)
		{
			FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			FARFilter Filter;
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			Filter.bRecursivePaths = true;
			TArray<FAssetData> Assets;
			ARM.Get().GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				if (A.AssetName.ToString().Equals(BPName, ESearchCase::IgnoreCase))
				{
					BP = Cast<UBlueprint>(NwiroSafeRegistryLoad(A));
					break;
				}
			}
		}
		if (!BP || !BP->GeneratedClass)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Blueprint not found: %s\"}"), *BPName);

		// GetDefaultObject() crashes outright (CoreUObject UObjectGlobals.cpp:3396
		// "InClass && InClass->ClassWithin && InClass->ClassConstructor") when the
		// generated class is half-constructed or never compiled. Validate state
		// before dereferencing so the caller sees an error instead of an editor crash.
		UClass* GenClass = BP->GeneratedClass;
		if (!GenClass->ClassWithin || !GenClass->ClassConstructor)
		{
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"GeneratedClass for %s is half-constructed (no ClassConstructor) — recompile the blueprint and retry. Asset path: %s\"}"),
				*BPName, *BP->GetPathName());
		}

		UObject* CDO = GenClass->GetDefaultObject();
		if (!CDO) return TEXT("{\"success\":false,\"error\":\"No CDO\"}");

		FProperty* Prop = BP->GeneratedClass->FindPropertyByName(FName(*PropName));
		if (!Prop)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Property not found: %s\"}"), *PropName);

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
		{
			FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetBlueprintProperty", "AI: Set Blueprint Property"), CDO);
			Tx.AlsoModify(BP);

			if (Prop->ImportText_Direct(*Value, ValuePtr, CDO, PPF_None))
			{
				CDO->MarkPackageDirty();
				FKismetEditorUtilities::CompileBlueprint(BP);
				const int32 VarIdx = FBlueprintEditorUtils::FindNewVariableIndex(BP, FName(*PropName));
				if (VarIdx != INDEX_NONE)
				{
					FProperty* P2 = BP->GeneratedClass ? BP->GeneratedClass->FindPropertyByName(FName(*PropName)) : Prop;
					UObject* CDO2 = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : CDO;
					FString Exported;
					if (P2 && CDO2)
					{
						void* V2 = P2->ContainerPtrToValuePtr<void>(CDO2);
						P2->ExportTextItem_Direct(Exported, V2, V2, CDO2, PPF_None);
					}
					if (Exported.IsEmpty()) Exported = Value;
					BP->NewVariables[VarIdx].DefaultValue = Exported;
					BP->MarkPackageDirty();
				}
				return FString::Printf(TEXT("{\"success\":true,\"property\":\"%s\",\"value\":\"%s\"}"), *PropName, *Value);
			}
			Tx.Cancel();
		}
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to set %s\"}"), *PropName);
	}

	// Utility
	if (ToolName == TEXT("clear_refs"))
	{
		FNwiroBlueprintTools::ClearNodeRefs();
		FNwiroMaterialTools::ClearExpressionRefs();
		return TEXT("{\"success\":true,\"message\":\"All refs cleared\"}");
	}

	// Data of the panel's Mesh events (meshy_task, tripo_task, asset3d_error), built with the
	// engine's JSON writer so any prompt or provider message (line breaks, quotes, backslashes)
	// is valid JSON. `Tool` names the tool that produced the event ("model" or "texture").
	// `RequestId` is an optional id from the Nwiro app (argument `requestId`), echoed so the app
	// ties the event to its request exactly; without one (older apps, direct MCP calls) the app
	// matches by prompt.
	auto MeshEventJson = [](const TSharedRef<FJsonObject>& Event, const TCHAR* Tool, const FString& RequestId) -> FString
	{
		Event->SetStringField(TEXT("tool"), Tool);
		if (!RequestId.IsEmpty()) Event->SetStringField(TEXT("requestId"), RequestId);
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Event, Writer);
		return Out;
	};

	// === 3D Generation: Meshy ===
	if (ToolName.EndsWith(TEXT("_managed")))
	{
		if (UNwiroBridge::Instance)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"%s\"}"), *UNwiroBridge::Instance->ManagedToolAdvice(ToolName));
		return TEXT("{\"success\":false,\"error\":\"Managed provider tools require the authenticated Nwiro renderer. Open the chat panel and select Nwiro Credits in Settings > Extensions.\"}");
	}
	if (ToolName == TEXT("generate_3d_model_meshy") || ToolName == TEXT("generate_3d_model_meshy_byok"))
	{
		if (UNwiroBridge::Instance && !UNwiroBridge::Instance->IsChatExtensionEnabled(TEXT("meshy")))
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Meshy 3D extension is not enabled for this chat. %s\"}"), *UNwiroBridge::Instance->ExtensionAccessHint(TEXT("meshy")));

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments\"}");

		FString Prompt = Args->GetStringField(TEXT("prompt"));
		if (Prompt.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"prompt is required\"}");

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("MESHY_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"Meshy API key not configured. Set it in Settings > Extensions > Meshy 3D.\"}");

		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		Body->SetStringField(TEXT("mode"), TEXT("preview"));
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetStringField(TEXT("ai_model"), TEXT("meshy-6"));
		FString Topology; Args->TryGetStringField(TEXT("topology"), Topology);
		double TargetPolycount = 30000; Args->TryGetNumberField(TEXT("target_polycount"), TargetPolycount);
		bool bShouldRemesh = true; Args->TryGetBoolField(TEXT("should_remesh"), bShouldRemesh);
		Body->SetStringField(TEXT("topology"), Topology.IsEmpty() ? TEXT("quad") : Topology);
		Body->SetNumberField(TEXT("target_polycount"), TargetPolycount);
		Body->SetBoolField(TEXT("should_remesh"), bShouldRemesh);
		const TArray<TSharedPtr<FJsonValue>>* RequestedFormats = nullptr;
		if (Args->TryGetArrayField(TEXT("target_formats"), RequestedFormats) && RequestedFormats && RequestedFormats->Num() > 0)
		{
			Body->SetArrayField(TEXT("target_formats"), *RequestedFormats);
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Formats;
			Formats.Add(MakeShareable(new FJsonValueString(TEXT("glb"))));
			Formats.Add(MakeShareable(new FJsonValueString(TEXT("fbx"))));
			Formats.Add(MakeShareable(new FJsonValueString(TEXT("obj"))));
			Body->SetArrayField(TEXT("target_formats"), Formats);
		}

		FString BodyStr;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, W);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(TEXT("https://api.meshy.ai/openapi/v2/text-to-3d"));
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		HttpReq->SetContentAsString(BodyStr);

		FString CapturedPrompt = Prompt;
		FString RequestId;
		Args->TryGetStringField(TEXT("requestId"), RequestId);
		RequestId = RequestId.Left(128);
		// Filled by Meshy's answer to the submit, which the tool waits for briefly below.
		const TSharedRef<bool> bAnswered = MakeShared<bool>(false);
		const TSharedRef<FString> AcceptedTaskId = MakeShared<FString>();
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedPrompt, RequestId, MeshEventJson, bAnswered, AcceptedTaskId](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
			{
				*bAnswered = true;
				auto PushError = [&RequestId, &MeshEventJson](const FString& Msg) {
					if (UNwiroBridge::Instance)
					{
						TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
						Event->SetStringField(TEXT("provider"), TEXT("meshy"));
						Event->SetStringField(TEXT("message"), Msg);
						UNwiroBridge::Instance->PushEvent(TEXT("asset3d_error"), MeshEventJson(Event, TEXT("model"), RequestId));
					}
				};
				if (!bSuccess || !Response.IsValid()) { UE_LOG(LogTemp, Error, TEXT("Nwiro: Meshy API request failed")); PushError(TEXT("Network error contacting Meshy")); return; }
				int32 Status = Response->GetResponseCode();
				FString ResponseBody = Response->GetContentAsString();
				if (Status < 200 || Status >= 300)
				{
					UE_LOG(LogTemp, Error, TEXT("Nwiro: Meshy API error %d: %s"), Status, *ResponseBody.Left(200));
					FString ErrMsg = FString::Printf(TEXT("Meshy %d"), Status);
					TSharedPtr<FJsonObject> ErrJson;
					TSharedRef<TJsonReader<>> ErrReader = TJsonReaderFactory<>::Create(ResponseBody);
					if (FJsonSerializer::Deserialize(ErrReader, ErrJson) && ErrJson.IsValid())
					{
						FString Msg;
						if (ErrJson->TryGetStringField(TEXT("message"), Msg) && !Msg.IsEmpty()) ErrMsg = Msg;
					}
					PushError(ErrMsg);
					return;
				}
				TSharedPtr<FJsonObject> RespJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
				FString TaskId;
				if (FJsonSerializer::Deserialize(Reader, RespJson) && RespJson.IsValid()
					&& RespJson->TryGetStringField(TEXT("result"), TaskId) && !TaskId.IsEmpty())
				{
					*AcceptedTaskId = TaskId;
					if (UNwiroBridge::Instance)
					{
						TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
						Event->SetStringField(TEXT("taskId"), TaskId);
						Event->SetStringField(TEXT("prompt"), CapturedPrompt);
						UNwiroBridge::Instance->PushEvent(TEXT("meshy_task"), MeshEventJson(Event, TEXT("model"), RequestId));
					}
				}
				else
				{
					// Accepted by HTTP status but without a job id: say so instead of leaving the
					// request without any answer.
					UE_LOG(LogTemp, Error, TEXT("Nwiro: Meshy answered without a job id: %s"), *ResponseBody.Left(200));
					PushError(TEXT("Meshy answered without a job id"));
				}
			});

		HttpReq->ProcessRequest();
		// >>> NWIRO_PARITY BEGIN: meshy-model-task-id
		// Meshy usually answers a submit within a second. A short wait (ticking the HTTP manager
		// on the game thread, as list_voices_elevenlabs does) lets this answer include the task
		// id, so the caller can texture the model later. If Meshy answers later, the app still
		// gets the job from the meshy_task event.
		const double SubmitDeadline = FPlatformTime::Seconds() + NwiroMeshyRetexture::SubmitWaitSeconds;
		while (!*bAnswered && FPlatformTime::Seconds() < SubmitDeadline)
		{
			FPlatformProcess::Sleep(0.02f);
			FHttpModule::Get().GetHttpManager().Tick(0.02f);
		}
		return NwiroMeshyRetexture::ModelSubmittedResult(*AcceptedTaskId);
		// <<< NWIRO_PARITY END: meshy-model-task-id
	}

	// === 3D Texture: Meshy ===
	// >>> NWIRO_PARITY BEGIN: meshy-retexture-handler
	if (ToolName == TEXT("generate_texture_meshy") || ToolName == TEXT("generate_texture_meshy_byok"))
	{
		if (UNwiroBridge::Instance && !UNwiroBridge::Instance->IsChatExtensionEnabled(TEXT("meshy")))
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Meshy 3D extension is not enabled for this chat. %s\"}"), *UNwiroBridge::Instance->ExtensionAccessHint(TEXT("meshy")));

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments\"}");

		NwiroMeshyRetexture::FRequest Request;
		FString RequestError;
		if (!NwiroMeshyRetexture::BuildRequest(Args, Request, RequestError))
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"%s\"}"), *RequestError);

		extern FString LoadNwiroSecret(const FString& Key);
		const FString ApiKey = LoadNwiroSecret(TEXT("MESHY_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"Meshy API key not configured.\"}");

		FString RequestId;
		Args->TryGetStringField(TEXT("requestId"), RequestId);
		RequestId = RequestId.Left(128);
		// Read the calling chat now and send every event of this call to it: the app may show
		// another chat by the time Meshy answers.
		FString ChatId;
		Args->TryGetStringField(TEXT("chatId"), ChatId);
		ChatId = ChatId.Left(128);
		if (ChatId.IsEmpty()) ChatId = NwiroMeshyRetexture::FallbackChatId();

		const FString InputTaskId = Request.InputTaskId;
		auto Push = [RequestId, ChatId, InputTaskId, MeshEventJson](const TCHAR* Type, const TSharedRef<FJsonObject>& Event)
		{
			if (!InputTaskId.IsEmpty()) Event->SetStringField(TEXT("inputTaskId"), InputTaskId);
			if (UNwiroBridge::Instance)
				UNwiroBridge::Instance->PushEvent(Type, MeshEventJson(Event, TEXT("texture"), RequestId), ChatId);
		};
		auto PushError = [Push](const FString& Msg)
		{
			TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
			Event->SetStringField(TEXT("provider"), TEXT("meshy"));
			Event->SetStringField(TEXT("message"), Msg);
			Push(TEXT("asset3d_error"), Event);
		};
		// Meshy's own message for a refused request, else the fallback.
		auto MeshyMessage = [](const FHttpResponsePtr& Response, const FString& Fallback) -> FString
		{
			TSharedPtr<FJsonObject> ErrJson;
			TSharedRef<TJsonReader<>> ErrReader = TJsonReaderFactory<>::Create(Response.IsValid() ? Response->GetContentAsString() : FString());
			FString Msg;
			if (FJsonSerializer::Deserialize(ErrReader, ErrJson) && ErrJson.IsValid()
				&& ErrJson->TryGetStringField(TEXT("message"), Msg) && !Msg.IsEmpty())
				return Msg;
			return Fallback;
		};

		auto Submit = [ApiKey, Request, Push, PushError, MeshyMessage]()
		{
			auto HttpReq = FHttpModule::Get().CreateRequest();
			HttpReq->SetURL(TEXT("https://api.meshy.ai/openapi/v1/retexture"));
			HttpReq->SetVerb(TEXT("POST"));
			HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
			HttpReq->SetContentAsString(Request.Body);
			const FString Prompt = Request.Prompt;
			HttpReq->OnProcessRequestComplete().BindLambda(
				[Prompt, Push, PushError, MeshyMessage](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
				{
					if (!bSuccess || !Response.IsValid()) { PushError(TEXT("Network error contacting Meshy")); return; }
					const int32 Status = Response->GetResponseCode();
					if (Status < 200 || Status >= 300)
					{
						PushError(MeshyMessage(Response, FString::Printf(TEXT("Meshy texture %d"), Status)));
						return;
					}
					TSharedPtr<FJsonObject> RespJson;
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
					FString TaskId;
					if (FJsonSerializer::Deserialize(Reader, RespJson) && RespJson.IsValid()
						&& RespJson->TryGetStringField(TEXT("result"), TaskId) && !TaskId.IsEmpty())
					{
						TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
						Event->SetStringField(TEXT("taskId"), TaskId);
						Event->SetStringField(TEXT("prompt"), Prompt);
						Push(TEXT("meshy_task"), Event);
					}
					else
					{
						PushError(TEXT("Meshy answered without a job id"));
					}
				});
			HttpReq->ProcessRequest();
		};

		if (InputTaskId.IsEmpty())
		{
			Submit();
		}
		else
		{
			// Check the source first: Meshy cannot retexture a Texture result, and its task
			// endpoint names each task's kind.
			auto CheckReq = FHttpModule::Get().CreateRequest();
			CheckReq->SetURL(FString::Printf(TEXT("https://api.meshy.ai/openapi/v1/retexture/%s"), *FGenericPlatformHttp::UrlEncode(InputTaskId)));
			CheckReq->SetVerb(TEXT("GET"));
			CheckReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
			CheckReq->OnProcessRequestComplete().BindLambda(
				[Submit, PushError, MeshyMessage](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
				{
					const int32 Status = bSuccess && Response.IsValid() ? Response->GetResponseCode() : 0;
					switch (NwiroMeshyRetexture::DecideSourceCheck(Status, Status != 0 ? Response->GetContentAsString() : FString()))
					{
					case NwiroMeshyRetexture::ESourceCheck::Submit:
						Submit();
						break;
					case NwiroMeshyRetexture::ESourceCheck::RefuseTexture:
						PushError(TEXT("This model is a Texture result; texture the model it came from instead"));
						break;
					case NwiroMeshyRetexture::ESourceCheck::Fail:
						PushError(Status == 0
							? FString(TEXT("Network error contacting Meshy"))
							: MeshyMessage(Response, FString::Printf(TEXT("Could not check the model at Meshy (%d)"), Status)));
						break;
					}
				});
			CheckReq->ProcessRequest();
		}

		return TEXT("{\"success\":true,\"status\":\"submitted\",\"message\":\"Texture generation submitted to Meshy. This is asynchronous: the textured model will appear in the chat and the Meshy panel when ready. Do NOT tell the user the texture is ready yet.\"}");
	}
	// <<< NWIRO_PARITY END: meshy-retexture-handler

	// === 3D Generation: Tripo ===
	if (ToolName == TEXT("generate_3d_model_tripo") || ToolName == TEXT("generate_3d_model_tripo_byok"))
	{
		if (UNwiroBridge::Instance && !UNwiroBridge::Instance->IsChatExtensionEnabled(TEXT("tripo")))
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"Tripo 3D extension is not enabled for this chat. %s\"}"), *UNwiroBridge::Instance->ExtensionAccessHint(TEXT("tripo")));

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments\"}");

		FString Prompt = Args->GetStringField(TEXT("prompt"));
		if (Prompt.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"prompt is required\"}");

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("TRIPO_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"Tripo API key not configured. Set it in Settings > Extensions > Tripo 3D.\"}");

		// Tripo V3: each task type has its own endpoint, so the body has no "type", and the model goes
		// in the required "model" field. V3 answers a new task in the shape read below: code,
		// data.task_id and message.
		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		Body->SetStringField(TEXT("prompt"), Prompt);
		Body->SetStringField(TEXT("model"), TEXT("v2.5-20250123"));

		FString BodyStr;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, W);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(TEXT("https://openapi.tripo3d.ai/v3/generation/text-to-model"));
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
		HttpReq->SetContentAsString(BodyStr);

		FString CapturedPrompt = Prompt;
		FString RequestId;
		Args->TryGetStringField(TEXT("requestId"), RequestId);
		RequestId = RequestId.Left(128);
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedPrompt, RequestId, MeshEventJson](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
			{
				auto PushError = [&RequestId, &MeshEventJson](const FString& Msg) {
					if (UNwiroBridge::Instance)
					{
						TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
						Event->SetStringField(TEXT("provider"), TEXT("tripo"));
						Event->SetStringField(TEXT("message"), Msg);
						UNwiroBridge::Instance->PushEvent(TEXT("asset3d_error"), MeshEventJson(Event, TEXT("model"), RequestId));
					}
				};
				if (!bSuccess || !Response.IsValid()) { UE_LOG(LogTemp, Error, TEXT("Nwiro: Tripo API request failed")); PushError(TEXT("Network error contacting Tripo")); return; }
				int32 Status = Response->GetResponseCode();
				FString ResponseBody = Response->GetContentAsString();
				auto ParseTripoMessage = [&ResponseBody](FString& OutMsg) {
					TSharedPtr<FJsonObject> J;
					TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ResponseBody);
					if (FJsonSerializer::Deserialize(R, J) && J.IsValid())
					{
						FString Msg;
						if (J->TryGetStringField(TEXT("message"), Msg) && !Msg.IsEmpty()) { OutMsg = Msg; return true; }
					}
					return false;
				};
				if (Status < 200 || Status >= 300)
				{
					UE_LOG(LogTemp, Error, TEXT("Nwiro: Tripo API error %d: %s"), Status, *ResponseBody.Left(200));
					FString ErrMsg = FString::Printf(TEXT("Tripo %d"), Status);
					ParseTripoMessage(ErrMsg);
					PushError(ErrMsg);
					return;
				}
				TSharedPtr<FJsonObject> RespJson;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
				if (FJsonSerializer::Deserialize(Reader, RespJson) && RespJson.IsValid())
				{
					int32 Code = 0;
					RespJson->TryGetNumberField(TEXT("code"), Code);
					const TSharedPtr<FJsonObject>* DataObj = nullptr;
					if (Code == 0 && RespJson->TryGetObjectField(TEXT("data"), DataObj) && DataObj && (*DataObj).IsValid())
					{
						FString TaskId;
						if ((*DataObj)->TryGetStringField(TEXT("task_id"), TaskId) && !TaskId.IsEmpty())
						{
							if (UNwiroBridge::Instance)
							{
								TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
								Event->SetStringField(TEXT("taskId"), TaskId);
								Event->SetStringField(TEXT("prompt"), CapturedPrompt);
								UNwiroBridge::Instance->PushEvent(TEXT("tripo_task"), MeshEventJson(Event, TEXT("model"), RequestId));
							}
						}
						else
						{
							PushError(TEXT("Tripo answered without a job id"));
						}
					}
					else
					{
						FString ErrMsg = FString::Printf(TEXT("Tripo error code %d"), Code);
						ParseTripoMessage(ErrMsg);
						PushError(ErrMsg);
					}
				}
				else
				{
					// Accepted by HTTP status but not readable as JSON.
					UE_LOG(LogTemp, Error, TEXT("Nwiro: Tripo answer could not be read: %s"), *ResponseBody.Left(200));
					PushError(TEXT("Tripo returned an answer that could not be read"));
				}
			});

		HttpReq->ProcessRequest();
		return TEXT("{\"success\":true,\"status\":\"submitted\",\"message\":\"Generation request submitted to Tripo. This is asynchronous — the model will appear in the Tripo panel when ready. Do NOT tell the user the model is ready yet.\"}");
	}

	// === ElevenLabs: Voices list (sync — small JSON response) ===
	if (ToolName == TEXT("list_voices_elevenlabs") || ToolName == TEXT("list_voices_elevenlabs_byok"))
	{
		if (UNwiroBridge::Instance && !UNwiroBridge::Instance->IsChatExtensionEnabled(TEXT("elevenlabs")))
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"ElevenLabs extension is not enabled for this chat. %s\"}"), *UNwiroBridge::Instance->ExtensionAccessHint(TEXT("elevenlabs")));

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("ELEVENLABS_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"ElevenLabs API key not configured. Set it in Settings > Extensions > ElevenLabs.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		FString Search, Category;
		int32 PageSize = 0;
		if (FJsonSerializer::Deserialize(R, Args) && Args.IsValid())
		{
			Args->TryGetStringField(TEXT("search"), Search);
			Args->TryGetStringField(TEXT("category"), Category);
			int32 Tmp; if (Args->TryGetNumberField(TEXT("pageSize"), Tmp)) PageSize = Tmp;
		}
		FString Url = TEXT("https://api.elevenlabs.io/v2/voices");
		TArray<FString> Q;
		if (!Search.IsEmpty())   Q.Add(FString::Printf(TEXT("search=%s"), *FGenericPlatformHttp::UrlEncode(Search)));
		if (!Category.IsEmpty()) Q.Add(FString::Printf(TEXT("category=%s"), *Category));
		if (PageSize > 0)        Q.Add(FString::Printf(TEXT("page_size=%d"), PageSize));
		if (Q.Num() > 0) Url += TEXT("?") + FString::Join(Q, TEXT("&"));

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(Url);
		HttpReq->SetVerb(TEXT("GET"));
		HttpReq->SetHeader(TEXT("xi-api-key"), ApiKey);

		bool bDone = false;
		FString ResponseBody;
		int32 ResponseStatus = 0;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[&bDone, &ResponseBody, &ResponseStatus](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
			{
				if (bOk && Resp.IsValid()) { ResponseBody = Resp->GetContentAsString(); ResponseStatus = Resp->GetResponseCode(); }
				bDone = true;
			});
		HttpReq->ProcessRequest();
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		while (!bDone && FPlatformTime::Seconds() < Deadline) { FPlatformProcess::Sleep(0.02f); FHttpModule::Get().GetHttpManager().Tick(0.02f); }
		if (!bDone) return TEXT("{\"success\":false,\"error\":\"ElevenLabs voices request timed out\"}");
		if (ResponseStatus < 200 || ResponseStatus >= 300)
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"ElevenLabs %d\",\"raw\":%s}"),
				ResponseStatus, *(ResponseBody.Left(400).Replace(TEXT("\""), TEXT("\\\""))));
		return FString::Printf(TEXT("{\"success\":true,\"voices_response\":%s}"), *ResponseBody);
	}

	// === ElevenLabs: TTS / SFX / Music (async, binary audio → save to disk + event) ===
	auto RunElevenLabsAudio = [&ArgsJson](const FString& Kind) -> FString
	{
		if (UNwiroBridge::Instance && !UNwiroBridge::Instance->IsChatExtensionEnabled(TEXT("elevenlabs")))
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"ElevenLabs extension is not enabled for this chat. %s\"}"), *UNwiroBridge::Instance->ExtensionAccessHint(TEXT("elevenlabs")));

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("ELEVENLABS_API_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"ElevenLabs API key not configured. Set it in Settings > Extensions > ElevenLabs.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(R, Args) || !Args.IsValid())
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments\"}");

		FString Url;
		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		FString HumanPrompt;
		if (Kind == TEXT("voice"))
		{
			FString Text, VoiceId, ModelId, LangCode;
			Args->TryGetStringField(TEXT("text"), Text);
			Args->TryGetStringField(TEXT("voiceId"), VoiceId);
			Args->TryGetStringField(TEXT("modelId"), ModelId);
			Args->TryGetStringField(TEXT("languageCode"), LangCode);
			if (Text.IsEmpty() || VoiceId.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"text + voiceId required\"}");
			Url = FString::Printf(TEXT("https://api.elevenlabs.io/v1/text-to-speech/%s"), *FGenericPlatformHttp::UrlEncode(VoiceId));
			Body->SetStringField(TEXT("text"), Text);
			Body->SetStringField(TEXT("model_id"), ModelId.IsEmpty() ? TEXT("eleven_multilingual_v2") : ModelId);
			if (!LangCode.IsEmpty()) Body->SetStringField(TEXT("language_code"), LangCode);
			HumanPrompt = Text.Left(120);
		}
		else if (Kind == TEXT("sfx"))
		{
			FString Prompt; double DurationSeconds = 0.0; double PromptInfluence = -1.0; bool bLoop = false;
			Args->TryGetStringField(TEXT("prompt"), Prompt);
			Args->TryGetNumberField(TEXT("durationSeconds"), DurationSeconds);
			Args->TryGetNumberField(TEXT("promptInfluence"), PromptInfluence);
			Args->TryGetBoolField(TEXT("loop"), bLoop);
			if (Prompt.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"prompt required\"}");
			Url = TEXT("https://api.elevenlabs.io/v1/sound-generation");
			Body->SetStringField(TEXT("text"), Prompt);
			if (DurationSeconds > 0) Body->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
			if (PromptInfluence >= 0) Body->SetNumberField(TEXT("prompt_influence"), PromptInfluence);
			if (bLoop) Body->SetBoolField(TEXT("loop"), true);
			HumanPrompt = Prompt.Left(120);
		}
		else if (Kind == TEXT("music"))
		{
			FString Prompt; int32 DurationMs = 0; bool bInstrumental = false;
			Args->TryGetStringField(TEXT("prompt"), Prompt);
			Args->TryGetNumberField(TEXT("durationMs"), DurationMs);
			Args->TryGetBoolField(TEXT("instrumental"), bInstrumental);
			if (Prompt.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"prompt required\"}");
			Url = TEXT("https://api.elevenlabs.io/v1/music");
			Body->SetStringField(TEXT("prompt"), Prompt);
			if (DurationMs > 0) Body->SetNumberField(TEXT("music_length_ms"), DurationMs);
			if (bInstrumental) Body->SetBoolField(TEXT("force_instrumental"), true);
			HumanPrompt = Prompt.Left(120);
		}
		else
		{
			return TEXT("{\"success\":false,\"error\":\"Unknown elevenlabs kind\"}");
		}

		// Optional id from the Nwiro app, echoed in the result events so the app ties the result to
		// its request exactly. Without one (older apps, direct MCP calls) the app matches by prompt.
		FString RequestId;
		Args->TryGetStringField(TEXT("requestId"), RequestId);
		RequestId = RequestId.Left(128);

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Wr = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, Wr);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(Url);
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("xi-api-key"), ApiKey);
		HttpReq->SetContentAsString(BodyStr);

		FString CapturedKind = Kind;
		FString CapturedPrompt = HumanPrompt;
		FString CapturedRequestId = RequestId;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedKind, CapturedPrompt, CapturedRequestId](FHttpRequestPtr, FHttpResponsePtr Resp, bool bOk)
			{
				// Both events are built with the engine's JSON writer, so any prompt or provider
				// message (line breaks, tabs, quotes, backslashes) produces valid JSON.
				auto EventJson = [&CapturedKind, &CapturedRequestId](const TSharedRef<FJsonObject>& Event) -> FString
				{
					Event->SetStringField(TEXT("kind"), CapturedKind);
					if (!CapturedRequestId.IsEmpty()) Event->SetStringField(TEXT("requestId"), CapturedRequestId);
					FString Out;
					TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
						TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
					FJsonSerializer::Serialize(Event, Writer);
					return Out;
				};
				auto PushError = [&EventJson](const FString& Msg)
				{
					if (!UNwiroBridge::Instance) return;
					TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
					Event->SetStringField(TEXT("provider"), TEXT("elevenlabs"));
					Event->SetStringField(TEXT("message"), Msg);
					UNwiroBridge::Instance->PushEvent(TEXT("elevenlabs_error"), EventJson(Event));
				};
				if (!bOk || !Resp.IsValid()) { PushError(TEXT("Network error contacting ElevenLabs")); return; }
				const int32 Status = Resp->GetResponseCode();
				if (Status < 200 || Status >= 300)
				{
					PushError(FString::Printf(TEXT("ElevenLabs %d: %s"), Status, *Resp->GetContentAsString().Left(200)));
					return;
				}
				const TArray<uint8>& Bytes = Resp->GetContent();
				if (Bytes.Num() == 0) { PushError(TEXT("ElevenLabs returned empty audio")); return; }
				if (UNwiroBridge::Instance)
				{
					TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
					Event->SetStringField(TEXT("prompt"), CapturedPrompt);
					Event->SetStringField(TEXT("ext"), TEXT("mp3"));
					Event->SetNumberField(TEXT("bytes"), Bytes.Num());
					Event->SetStringField(TEXT("b64"), FBase64::Encode(Bytes));
					UNwiroBridge::Instance->PushEvent(TEXT("elevenlabs_task"), EventJson(Event));
				}
			});

		HttpReq->ProcessRequest();
		return FString::Printf(
			TEXT("{\"success\":true,\"status\":\"submitted\",\"kind\":\"%s\",\"message\":\"ElevenLabs %s generation submitted. The audio will appear in the ElevenLabs side panel where the user can preview and import. Do NOT tell the user the audio is ready yet — they will see it in the side panel.\"}"),
			*Kind, *Kind);
	};

	if (ToolName == TEXT("generate_voice_elevenlabs") || ToolName == TEXT("generate_voice_elevenlabs_byok")) return RunElevenLabsAudio(TEXT("voice"));
	if (ToolName == TEXT("generate_sfx_elevenlabs") || ToolName == TEXT("generate_sfx_elevenlabs_byok"))   return RunElevenLabsAudio(TEXT("sfx"));
	if (ToolName == TEXT("generate_music_elevenlabs") || ToolName == TEXT("generate_music_elevenlabs_byok")) return RunElevenLabsAudio(TEXT("music"));

	// === fal.ai material generation — meshy pattern ===
	if (ToolName == TEXT("generate_material_fal") || ToolName == TEXT("generate_material_fal_byok"))
	{
		if (UNwiroBridge::Instance && !UNwiroBridge::Instance->IsChatExtensionEnabled(TEXT("fal")))
			return FString::Printf(TEXT("{\"success\":false,\"error\":\"fal.ai extension is not enabled for this chat. %s\"}"), *UNwiroBridge::Instance->ExtensionAccessHint(TEXT("fal")));

		extern FString LoadNwiroSecret(const FString& Key);
		FString ApiKey = LoadNwiroSecret(TEXT("FAL_KEY"));
		if (ApiKey.IsEmpty())
			return TEXT("{\"success\":false,\"error\":\"fal.ai API key not configured. Set it in Settings > Extensions > fal.ai.\"}");

		TSharedPtr<FJsonObject> Args;
		TSharedRef<TJsonReader<>> ArgsReader = TJsonReaderFactory<>::Create(ArgsJson);
		if (!FJsonSerializer::Deserialize(ArgsReader, Args) || !Args.IsValid())
		{
			return TEXT("{\"success\":false,\"error\":\"Invalid arguments JSON\"}");
		}
		FString Prompt;
		Args->TryGetStringField(TEXT("prompt"), Prompt);
		if (Prompt.IsEmpty()) return TEXT("{\"success\":false,\"error\":\"prompt required\"}");

		const FString TaskId = FString::Printf(TEXT("fal_%lld_%d"), FDateTime::UtcNow().ToUnixTimestamp(), FMath::RandRange(0, 99999));
		if (UNwiroBridge::Instance)
		{
			const FString EscPrompt = Prompt.Left(160).Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			UNwiroBridge::Instance->PushEvent(TEXT("fal_pending"),
				FString::Printf(TEXT("{\"id\":\"%s\",\"prompt\":\"%s\"}"), *TaskId, *EscPrompt));
		}

		TSharedRef<FJsonObject> Body = MakeShareable(new FJsonObject);
		Body->SetStringField(TEXT("prompt"), Prompt);
		FString ImageUrl; if (Args->TryGetStringField(TEXT("imageUrl"), ImageUrl) && !ImageUrl.IsEmpty()) Body->SetStringField(TEXT("image_url"), ImageUrl);
		FString ImageSize; if (Args->TryGetStringField(TEXT("imageSize"), ImageSize) && !ImageSize.IsEmpty()) Body->SetStringField(TEXT("image_size"), ImageSize);
		FString OutputFormat; if (Args->TryGetStringField(TEXT("outputFormat"), OutputFormat) && !OutputFormat.IsEmpty()) Body->SetStringField(TEXT("output_format"), OutputFormat);
		FString TilingMode; if (Args->TryGetStringField(TEXT("tilingMode"), TilingMode) && !TilingMode.IsEmpty()) Body->SetStringField(TEXT("tiling_mode"), TilingMode);
		double NumD = 0;
		if (Args->TryGetNumberField(TEXT("strength"), NumD)) Body->SetNumberField(TEXT("strength"), NumD);
		if (Args->TryGetNumberField(TEXT("numInferenceSteps"), NumD)) Body->SetNumberField(TEXT("num_inference_steps"), NumD);
		if (Args->TryGetNumberField(TEXT("numImages"), NumD)) Body->SetNumberField(TEXT("num_images"), NumD);
		if (Args->TryGetNumberField(TEXT("seed"), NumD)) Body->SetNumberField(TEXT("seed"), NumD);
		if (Args->TryGetNumberField(TEXT("upscaleFactor"), NumD)) Body->SetNumberField(TEXT("upscale_factor"), NumD);
		const TArray<TSharedPtr<FJsonValue>>* MapsArr = nullptr;
		if (Args->TryGetArrayField(TEXT("maps"), MapsArr) && MapsArr)
		{
			Body->SetArrayField(TEXT("maps"), *MapsArr);
		}

		FString BodyStr;
		TSharedRef<TJsonWriter<>> Wr = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body, Wr);

		auto HttpReq = FHttpModule::Get().CreateRequest();
		HttpReq->SetURL(TEXT("https://fal.run/fal-ai/patina/material"));
		HttpReq->SetVerb(TEXT("POST"));
		HttpReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpReq->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Key %s"), *ApiKey));
		HttpReq->SetContentAsString(BodyStr);
		// fal.run answers only when the material is done and sends nothing meanwhile. Unreal drops a
		// connection after 30 s without data by default (HttpActivityTimeout in BaseEngine.ini), which
		// cut slower generations off as a "network error".
		HttpReq->SetActivityTimeout(300.0f);
		HttpReq->SetTimeout(600.0f);
		const double StartSeconds = FPlatformTime::Seconds();

		FString CapturedTaskId = TaskId;
		HttpReq->OnProcessRequestComplete().BindLambda(
			[CapturedTaskId, StartSeconds](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bOk)
			{
				auto PushError = [&CapturedTaskId](const FString& Msg)
				{
					UE_LOG(LogTemp, Warning, TEXT("[Nwiro][fal] error: %s"), *Msg);
					if (UNwiroBridge::Instance)
					{
						const FString Esc = Msg.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
						UNwiroBridge::Instance->PushEvent(TEXT("fal_error"),
							FString::Printf(TEXT("{\"id\":\"%s\",\"message\":\"%s\"}"), *CapturedTaskId, *Esc));
					}
				};
				if (!bOk || !Resp.IsValid())
				{
					// Why it failed, and after how long (a cut for silence is reported as a connection error).
					const EHttpFailureReason Reason = Req.IsValid() ? Req->GetFailureReason() : EHttpFailureReason::Other;
					UE_LOG(LogTemp, Warning, TEXT("[Nwiro][fal] request failed after %.1f s: %s"),
						FPlatformTime::Seconds() - StartSeconds, LexToString(Reason));
					PushError(Reason == EHttpFailureReason::TimedOut ? TEXT("fal.ai did not answer in time (10 minutes)")
						: Reason == EHttpFailureReason::ConnectionError ? TEXT("The connection to fal.ai failed or was dropped")
						: TEXT("Network error contacting fal.ai"));
					return;
				}
				const int32 Status = Resp->GetResponseCode();
				// Every answer, with how long fal took (status and size only: no prompt, key or links).
				UE_LOG(LogTemp, Display, TEXT("[Nwiro][fal] answered in %.1f s: HTTP %d, %d bytes"),
					FPlatformTime::Seconds() - StartSeconds, Status, Resp->GetContentLength());
				if (Status < 200 || Status >= 300)
				{
					PushError(FString::Printf(TEXT("fal.ai %d: %s"), Status, *Resp->GetContentAsString().Left(200)));
					return;
				}

				TSharedPtr<FJsonObject> RespJson;
				TSharedRef<TJsonReader<>> RespReader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
				if (!FJsonSerializer::Deserialize(RespReader, RespJson) || !RespJson.IsValid())
				{
					PushError(TEXT("fal.ai returned non-JSON")); return;
				}
				const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
				if (!RespJson->TryGetArrayField(TEXT("images"), Images) || !Images || Images->Num() == 0)
				{
					PushError(TEXT("fal.ai response had no images")); return;
				}
				const int32 ImageCount = Images->Num();
				UE_LOG(LogTemp, Display, TEXT("[Nwiro][fal] %d image URLs"), ImageCount);

				FString UrlsJson = TEXT("[");
				bool bFirst = true;
				for (const TSharedPtr<FJsonValue>& V : *Images)
				{
					const TSharedPtr<FJsonObject> Obj = V->AsObject();
					if (!Obj.IsValid()) continue;
					FString Url; if (!Obj->TryGetStringField(TEXT("url"), Url) || Url.IsEmpty()) continue;
					FString MapType; Obj->TryGetStringField(TEXT("map_type"), MapType);
					FString CType;   Obj->TryGetStringField(TEXT("content_type"), CType);
					const FString Ext = CType.Contains(TEXT("jpeg")) || CType.Contains(TEXT("jpg")) ? TEXT("jpg")
									   : CType.Contains(TEXT("webp")) ? TEXT("webp") : TEXT("png");
					const FString EscUrl = Url.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
					const FString EscType = MapType.Replace(TEXT("\""), TEXT("\\\""));
					if (!bFirst) UrlsJson += TEXT(",");
					UrlsJson += FString::Printf(TEXT("{\"mapType\":\"%s\",\"ext\":\"%s\",\"url\":\"%s\"}"), *EscType, *Ext, *EscUrl);
					bFirst = false;
				}
				UrlsJson += TEXT("]");
				if (UNwiroBridge::Instance)
				{
					UNwiroBridge::Instance->PushEvent(TEXT("fal_urls"),
						FString::Printf(TEXT("{\"id\":\"%s\",\"urls\":%s}"), *CapturedTaskId, *UrlsJson));
				}
			});

		HttpReq->ProcessRequest();
		return FString::Printf(TEXT("{\"success\":true,\"status\":\"submitted\",\"id\":\"%s\",\"message\":\"fal.ai material generation submitted. The maps will appear in the fal.ai side panel where the user can preview and import. Do NOT tell the user the textures are ready yet — they will see them in the side panel.\"}"), *TaskId);
	}

	// Not a built-in tool — try registered third-party providers before failing.
	{
		FString ProviderResult;
		if (FNwiroToolRegistry::TryDispatch(ToolName, ArgsJson, ProviderResult))
		{
			return ProviderResult;
		}
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Unknown tool: %s\"}"), *ToolName);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroSaveAssetProofSpec.inl"
#include "Tests/NwiroSaveAssetAcceptanceSpec.inl"
#endif

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroMeshyRetextureSpec.inl"
#endif
