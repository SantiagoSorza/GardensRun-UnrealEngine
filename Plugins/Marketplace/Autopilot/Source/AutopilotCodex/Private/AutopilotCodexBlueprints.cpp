// The AutopilotCodex commandlet's Blueprints (blueprints/*.txt): a Blueprint's components, variables, widgets and
// class defaults, and its graphs as pseudo-code (FGraphRenderer).

#include "AutopilotCodexInternal.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ActorComponent.h"
#include "Curves/CurveFloat.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "K2Node.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_InputKey.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace AutopilotCodex
{
	// ---------------------------------------------------------------- graph renderer

	class FGraphRenderer
	{
	public:
		FGraphRenderer(FOut& InOut, int32 InMaxLines)
			: Out(InOut)
			, StartLine(InOut.Lines.Num())
			, MaxLines(InMaxLines)
		{
		}

		/** Renders a graph. Kind is "Graph" for event graphs (entries listed under a header), else the entry is the header. */
		void Render(UEdGraph* G, const FString& Kind, const FString& Suffix)
		{
			TArray<UEdGraphNode*> Entries;
			TArray<FString> Comments;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (!N)
				{
					continue;
				}
				if (const UEdGraphNode_Comment* C = Cast<UEdGraphNode_Comment>(N))
				{
					const FString T = OneLine(C->NodeComment);
					if (!T.IsEmpty() && Comments.Num() < 12)
					{
						Comments.Add(TEXT("\"") + Cap(T, 140) + TEXT("\""));
					}
					continue;
				}
				if (IsEntry(N))
				{
					Entries.Add(N);
				}
			}
			// Function entries first, then the rest in graph order.
			// Function entries, then input events (so a key's handler prints in full under it), then other events,
			// then custom events (usually called from elsewhere); graph order within each group.
			auto Rank = [](const UEdGraphNode& N)
			{
				if (N.IsA<UK2Node_FunctionEntry>()) return 0;
				if (N.GetClass()->GetName().Contains(TEXT("Input"))) return 1;
				if (N.IsA<UK2Node_CustomEvent>()) return 3;
				return 2;
			};
			Entries.StableSort([&Rank](const UEdGraphNode& A, const UEdGraphNode& B) { return Rank(A) < Rank(B); });

			const bool bEventGraph = Kind == TEXT("Graph");
			if (bEventGraph)
			{
				Emit(0, TEXT("Graph ") + G->GetName() + Suffix + TEXT(":"));
				if (Comments.Num() > 0)
				{
					Emit(1, TEXT("Comments: ") + FString::Join(Comments, TEXT("; ")));
				}
				bCountBody = true;
				bool bAny = false;
				for (UEdGraphNode* E : Entries)
				{
					if (!HasLinkedExecOut(E) || Printed.Contains(E))
					{
						continue;
					}
					bAny = true;
					RenderEntry(E, 1, FString());
				}
				if (!bAny)
				{
					RenderSinks(G, 1);
				}
				return;
			}

			// Function / macro / collapsed graph: the entry node's line is the header.
			UEdGraphNode* Main = Entries.Num() > 0 ? Entries[0] : nullptr;
			if (!Main)
			{
				Emit(0, Kind + TEXT(" ") + G->GetName() + Suffix + TEXT(":"));
				if (Comments.Num() > 0)
				{
					Emit(1, TEXT("Comments: ") + FString::Join(Comments, TEXT("; ")));
				}
				bCountBody = true;
				RenderSinks(G, 1);
				return;
			}
			FString Header = Kind + TEXT(" ") + G->GetName() + TEXT("(") + ParamList(Main, true) + TEXT(")");
			FString Returns = ReturnList(G);
			if (!Returns.IsEmpty())
			{
				Header += TEXT(" -> (") + Returns + TEXT(")");
			}
			if (const UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Main))
			{
				const int32 Flags = FE->GetFunctionFlags();
				TArray<FString> F;
				if (Flags & FUNC_BlueprintPure) F.Add(TEXT("pure"));
				if (Flags & FUNC_Const) F.Add(TEXT("const"));
				if (Flags & FUNC_Private) F.Add(TEXT("private"));
				if (Flags & FUNC_Net) F.Add(TEXT("replicated"));
				if (F.Num() > 0)
				{
					Header += TEXT(" [") + FString::Join(F, TEXT(", ")) + TEXT("]");
				}
			}
			Emit(0, Header + Suffix + TEXT(":"));
			if (const UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Main))
			{
				if (FE->LocalVariables.Num() > 0)
				{
					TArray<FString> L;
					for (const FBPVariableDescription& V : FE->LocalVariables)
					{
						FString S = V.VarName.ToString() + TEXT(": ") + PinTypeStr(V.VarType);
						if (!V.DefaultValue.IsEmpty())
						{
							S += TEXT(" = ") + Clean(V.DefaultValue, 80);
						}
						L.Add(S);
					}
					Emit(1, TEXT("Locals: ") + FString::Join(L, TEXT(", ")));
				}
			}
			if (Comments.Num() > 0)
			{
				Emit(1, TEXT("Comments: ") + FString::Join(Comments, TEXT("; ")));
			}
			Printed.Add(Main);
			bCountBody = true;
			RenderExecOuts(Main, 1);
			for (int32 i = 1; i < Entries.Num(); ++i)
			{
				if (HasLinkedExecOut(Entries[i]) && !Printed.Contains(Entries[i]))
				{
					RenderEntry(Entries[i], 1, FString());
				}
			}
			if (!HasLinkedExecOut(Main) && Entries.Num() == 1)
			{
				// Pure function or signature only: show what the return node computes, if anything.
				RenderSinks(G, 1);
			}
		}

		/** True when the graph rendered anything beyond its header (events, statements, results). */
		bool HasBody() const { return BodyLines > 0; }

	private:
		FOut& Out;
		int32 StartLine;
		int32 MaxLines;
		bool bCapped = false;
		bool bCountBody = false;
		int32 BodyLines = 0;
		TSet<const UEdGraphNode*> Printed;

		void Emit(int32 Indent, const FString& Text)
		{
			if (Out.Lines.Num() - StartLine >= MaxLines)
			{
				if (!bCapped)
				{
					bCapped = true;
					Out.Add(Indent, TEXT("... (graph truncated)"));
				}
				return;
			}
			if (bCountBody)
			{
				++BodyLines;
			}
			Out.Add(Indent, Text);
		}

		bool Stopped() const
		{
			return bCapped || Out.Full();
		}

		static bool IsExec(const UEdGraphPin* P)
		{
			return P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
		}

		static bool Visible(const UEdGraphPin* P)
		{
			return P && !P->bHidden && !P->bOrphanedPin;
		}

		static void ExecPins(const UEdGraphNode* N, EEdGraphPinDirection Dir, TArray<UEdGraphPin*>& Result)
		{
			for (UEdGraphPin* P : N->Pins)
			{
				if (Visible(P) && P->Direction == Dir && IsExec(P))
				{
					Result.Add(P);
				}
			}
		}

		static bool IsPure(const UEdGraphNode* N)
		{
			for (const UEdGraphPin* P : N->Pins)
			{
				if (P && IsExec(P) && !P->bHidden)
				{
					return false;
				}
			}
			return true;
		}

		static bool IsEntry(const UEdGraphNode* N)
		{
			if (N->IsA<UK2Node_Knot>())
			{
				return false;
			}
			TArray<UEdGraphPin*> In, OutPins;
			ExecPins(N, EGPD_Input, In);
			ExecPins(N, EGPD_Output, OutPins);
			return In.Num() == 0 && OutPins.Num() > 0;
		}

		static bool HasLinkedExecOut(const UEdGraphNode* N)
		{
			TArray<UEdGraphPin*> OutPins;
			ExecPins(N, EGPD_Output, OutPins);
			for (const UEdGraphPin* P : OutPins)
			{
				if (P->LinkedTo.Num() > 0)
				{
					return true;
				}
			}
			return false;
		}

		static FString PinLabel(const UEdGraphNode* N, const UEdGraphPin* P)
		{
			FString L = OneLine(N->GetPinDisplayName(P).ToString());
			if (L.IsEmpty())
			{
				L = P->PinName.ToString();
			}
			return L;
		}

		static FString Title(const UEdGraphNode* N)
		{
			if (const UK2Node_Variable* V = Cast<UK2Node_Variable>(N))
			{
				if (N->IsA<UK2Node_VariableGet>())
				{
					return V->GetVarNameString();
				}
			}
			if (N->IsA<UK2Node_FunctionResult>())
			{
				return TEXT("Return");
			}
			if (const UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(N))
			{
				FString T = TEXT("Custom Event ") + CE->CustomFunctionName.ToString();
				const uint32 F = CE->FunctionFlags;
				if (F & FUNC_Net)
				{
					FString Net = (F & FUNC_NetServer) ? TEXT("run on server") : (F & FUNC_NetClient) ? TEXT("run on owning client") : (F & FUNC_NetMulticast) ? TEXT("multicast") : TEXT("replicated");
					if (F & FUNC_NetReliable)
					{
						Net += TEXT(", reliable");
					}
					T += TEXT(" [") + Net + TEXT("]");
				}
				return T;
			}
			if (const UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(N))
			{
				// Blueprint-defined functions and events by their own name, so calls grep to their definitions.
				const UFunction* F = CF->GetTargetFunction();
				if (F && Cast<UBlueprintGeneratedClass>(F->GetOwnerClass()) && !N->IsA<UK2Node_CallParentFunction>())
				{
					return F->GetName();
				}
			}
			FString T = N->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			int32 NL;
			if (T.FindChar(TEXT('\n'), NL))
			{
				T = T.Left(NL);
			}
			T = OneLine(T);
			if (T.IsEmpty())
			{
				T = OneLine(N->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
			if (T.IsEmpty())
			{
				T = N->GetClass()->GetName();
			}
			if (N->IsA<UK2Node_InputKey>())
			{
				T = TEXT("Key ") + T;
			}
			return T;
		}

		static FString Label(const UEdGraphNode* N, const UEdGraphPin* EnteredBy)
		{
			FString T = Title(N);
			TArray<UEdGraphPin*> In;
			ExecPins(N, EGPD_Input, In);
			if (EnteredBy && In.Num() > 1)
			{
				T += TEXT(" [") + PinLabel(N, EnteredBy) + TEXT("]");
			}
			return T;
		}

		/** The node an exec output leads to, passing through reroute knots. */
		static UEdGraphNode* Follow(const UEdGraphPin* From, UEdGraphPin*& OutIn)
		{
			const UEdGraphPin* P = From;
			for (int32 Guard = 0; Guard < 64 && P; ++Guard)
			{
				if (P->LinkedTo.Num() == 0 || !P->LinkedTo[0])
				{
					return nullptr;
				}
				UEdGraphPin* In = P->LinkedTo[0];
				UEdGraphNode* N = In->GetOwningNode();
				if (const UK2Node_Knot* K = Cast<UK2Node_Knot>(N))
				{
					P = K->GetOutputPin();
					continue;
				}
				OutIn = In;
				return N;
			}
			return nullptr;
		}

		FString ParamList(const UEdGraphNode* N, bool bTypes, bool bOnlyLinked = false) const
		{
			TArray<FString> Params;
			for (const UEdGraphPin* P : N->Pins)
			{
				if (!Visible(P) || P->Direction != EGPD_Output || IsExec(P) || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate
					|| (bOnlyLinked && P->LinkedTo.Num() == 0))
				{
					continue;
				}
				FString S = PinLabel(N, P);
				if (bTypes)
				{
					S += TEXT(": ") + PinTypeStr(P->PinType);
				}
				Params.Add(S);
			}
			return FString::Join(Params, TEXT(", "));
		}

		static FString ReturnList(const UEdGraph* G)
		{
			for (const UEdGraphNode* N : G->Nodes)
			{
				if (N && N->IsA<UK2Node_FunctionResult>())
				{
					TArray<FString> R;
					for (const UEdGraphPin* P : N->Pins)
					{
						if (Visible(P) && P->Direction == EGPD_Input && !IsExec(P))
						{
							R.Add(PinLabel(N, P) + TEXT(": ") + PinTypeStr(P->PinType));
						}
					}
					return FString::Join(R, TEXT(", "));
				}
			}
			return FString();
		}

		void RenderEntry(UEdGraphNode* E, int32 Indent, const FString& Prefix)
		{
			Printed.Add(E);
			const bool bTypes = E->IsA<UK2Node_FunctionEntry>() || E->IsA<UK2Node_CustomEvent>() || E->IsA<UK2Node_Tunnel>();
			FString Line = Prefix + Title(E) + TEXT("(") + ParamList(E, bTypes, !bTypes) + TEXT(")");
			if (!E->IsNodeEnabled())
			{
				Line += TEXT(" [disabled]");
			}
			Emit(Indent, Line + TEXT(":"));
			RenderExecOuts(E, Indent + 1);
		}

		/** After a node's line: continue in sequence (one exec out) or print each linked exec out as a labeled block. */
		void RenderExecOuts(UEdGraphNode* N, int32 Indent)
		{
			TArray<UEdGraphPin*> Outs;
			ExecPins(N, EGPD_Output, Outs);
			if (Outs.Num() == 1)
			{
				RenderChain(Outs[0], Indent);
				return;
			}
			for (UEdGraphPin* P : Outs)
			{
				if (P->LinkedTo.Num() == 0 || Stopped())
				{
					continue;
				}
				Emit(Indent, PinLabel(N, P) + TEXT(":"));
				RenderChain(P, Indent + 1);
			}
		}

		void RenderChain(UEdGraphPin* From, int32 Indent)
		{
			UEdGraphPin* Cur = From;
			while (Cur && !Stopped())
			{
				UEdGraphPin* In = nullptr;
				UEdGraphNode* N = Follow(Cur, In);
				if (!N)
				{
					return;
				}
				if (Printed.Contains(N))
				{
					FString Where = Cap(Call(N, false, MaxExprDepth - 2), 120);
					if (In && Label(N, In) != Title(N))
					{
						Where += TEXT(" [") + PinLabel(N, In) + TEXT("]");
					}
					Emit(Indent, TEXT("-> (continues at ") + Where + TEXT(")"));
					return;
				}
				Printed.Add(N);
				if (Indent > MaxIndent)
				{
					Emit(Indent, TEXT("... (nested too deep)"));
					return;
				}
				FString Line = Call(N, false, 0);
				TArray<UEdGraphPin*> Ins;
				ExecPins(N, EGPD_Input, Ins);
				if (In && Ins.Num() > 1)
				{
					Line += TEXT(" [") + PinLabel(N, In) + TEXT("]");
				}
				if (!N->IsNodeEnabled())
				{
					Line += TEXT(" [disabled]");
				}
				Emit(Indent, Line);
				TArray<UEdGraphPin*> Outs;
				ExecPins(N, EGPD_Output, Outs);
				if (Outs.Num() == 1)
				{
					Cur = Outs[0];
					continue;
				}
				for (UEdGraphPin* P : Outs)
				{
					if (P->LinkedTo.Num() == 0 || Stopped())
					{
						continue;
					}
					Emit(Indent + 1, PinLabel(N, P) + TEXT(":"));
					RenderChain(P, Indent + 2);
				}
				return;
			}
		}

		/** Graphs without exec flow (anim graphs, transition rules): render the nodes nothing reads from. */
		void RenderSinks(UEdGraph* G, int32 Indent)
		{
			int32 Lines = 0;
			for (UEdGraphNode* N : G->Nodes)
			{
				if (!N || N->IsA<UEdGraphNode_Comment>() || Printed.Contains(N) || Stopped())
				{
					continue;
				}
				bool bOutLinked = false;
				bool bInLinked = false;
				for (const UEdGraphPin* P : N->Pins)
				{
					if (!P || P->LinkedTo.Num() == 0)
					{
						continue;
					}
					(P->Direction == EGPD_Output ? bOutLinked : bInLinked) = true;
				}
				if (bOutLinked || !bInLinked)
				{
					continue;
				}
				Printed.Add(N);
				Emit(Indent, Call(N, true, 0));
				if (++Lines >= MaxDataGraphLines)
				{
					Emit(Indent, TEXT("..."));
					return;
				}
			}
		}

		FString PinDefault(const UEdGraphPin* P) const
		{
			if (P->DefaultObject)
			{
				return P->DefaultObject->GetName();
			}
			if (!P->DefaultTextValue.IsEmpty())
			{
				return TEXT("\"") + Cap(OneLine(P->DefaultTextValue.ToString()), 160) + TEXT("\"");
			}
			const FString& D = P->DefaultValue;
			if (D.IsEmpty())
			{
				return FString();
			}
			if (P->bAdvancedView && D == P->AutogeneratedDefaultValue)
			{
				return FString();
			}
			if (D == TEXT("None") && (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Name || P->PinType.PinCategory == UEdGraphSchema_K2::PC_Object))
			{
				return FString();
			}
			const FName Cat = P->PinType.PinCategory;
			if (Cat == UEdGraphSchema_K2::PC_String)
			{
				return TEXT("\"") + Cap(OneLine(D), 160) + TEXT("\"");
			}
			if (Cat == UEdGraphSchema_K2::PC_Byte || Cat == UEdGraphSchema_K2::PC_Enum)
			{
				return EnumNameToDisplay(Cast<UEnum>(P->PinType.PinSubCategoryObject.Get()), D);
			}
			if (Cat == UEdGraphSchema_K2::PC_Struct && !D.StartsWith(TEXT("(")) && D.Contains(TEXT(",")))
			{
				return TEXT("(") + Clean(D, 160).Replace(TEXT(" "), TEXT("")) + TEXT(")");
			}
			return Clean(D, 160);
		}

		FString Expr(const UEdGraphPin* InputPin, int32 Depth)
		{
			if (InputPin->LinkedTo.Num() > 0 && InputPin->LinkedTo[0])
			{
				return Cap(SourceExpr(InputPin->LinkedTo[0], Depth), MaxExprLen);
			}
			return PinDefault(InputPin);
		}

		FString SourceExpr(const UEdGraphPin* OutPin, int32 Depth)
		{
			const UEdGraphNode* N = OutPin->GetOwningNode();
			if (Depth > MaxExprDepth)
			{
				return TEXT("...");
			}
			if (const UK2Node_Knot* K = Cast<UK2Node_Knot>(N))
			{
				const UEdGraphPin* KIn = K->GetInputPin();
				return KIn ? Expr(KIn, Depth + 1) : FString(TEXT("?"));
			}
			if (N->IsA<UK2Node_Self>())
			{
				return TEXT("self");
			}
			if (const UK2Node_CreateDelegate* CD = Cast<UK2Node_CreateDelegate>(N))
			{
				return TEXT("Event ") + CD->GetFunctionName().ToString();
			}
			if (const UK2Node_DynamicCast* DC = Cast<UK2Node_DynamicCast>(N))
			{
				if (OutPin == DC->GetCastResultPin())
				{
					const UEdGraphPin* Src = DC->GetCastSourcePin();
					FString ClassName = DC->TargetType ? DC->TargetType->GetName() : FString(TEXT("?"));
					ClassName.RemoveFromEnd(TEXT("_C"));
					return TEXT("(") + (Src ? Expr(Src, Depth + 1) : FString(TEXT("?"))) + TEXT(" as ") + ClassName + TEXT(")");
				}
			}
			if (N->IsA<UK2Node_VariableGet>() || N->IsA<UK2Node_VariableSet>())
			{
				const UK2Node_Variable* V = CastChecked<UK2Node_Variable>(N);
				const FString Target = TargetPrefix(N, Depth);
				return Target + V->GetVarNameString();
			}
			if (IsEntry(N) || N->IsA<UK2Node_Tunnel>())
			{
				if (OutPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
				{
					return Title(N);
				}
				return PinLabel(N, OutPin);
			}
			if (IsPure(N))
			{
				if (const UK2Node_BreakStruct* BS = Cast<UK2Node_BreakStruct>(N))
				{
					for (const UEdGraphPin* P : BS->Pins)
					{
						if (P && P->Direction == EGPD_Input && !IsExec(P))
						{
							return Expr(P, Depth + 1) + TEXT(".") + PinLabel(N, OutPin);
						}
					}
				}
				const FString C = Call(N, true, Depth);
				int32 DataOuts = 0;
				for (const UEdGraphPin* P : N->Pins)
				{
					if (Visible(P) && P->Direction == EGPD_Output && !IsExec(P))
					{
						++DataOuts;
					}
				}
				return DataOuts > 1 ? C + TEXT(".") + PinLabel(N, OutPin) : C;
			}
			return Title(N) + TEXT(".") + PinLabel(N, OutPin);
		}

		FString TargetPrefix(const UEdGraphNode* N, int32 Depth)
		{
			for (const UEdGraphPin* P : N->Pins)
			{
				if (P && P->Direction == EGPD_Input && P->PinName == UEdGraphSchema_K2::PN_Self && P->LinkedTo.Num() > 0)
				{
					const FString T = Expr(P, Depth + 1);
					if (!T.IsEmpty() && T != TEXT("self"))
					{
						return T + TEXT(".");
					}
				}
			}
			return FString();
		}

		/** Title(Pin = expr, ...) with "Target." in front when the target is not self. */
		FString Call(const UEdGraphNode* N, bool bPure, int32 Depth)
		{
			if (N->IsA<UK2Node_VariableGet>())
			{
				const FString Name = TargetPrefix(N, Depth) + CastChecked<UK2Node_Variable>(N)->GetVarNameString();
				return IsPure(N) ? Name : TEXT("Validated Get ") + Name;
			}
			if (const UK2Node_VariableSet* VS = Cast<UK2Node_VariableSet>(N))
			{
				FString Value;
				for (const UEdGraphPin* P : N->Pins)
				{
					if (Visible(P) && P->Direction == EGPD_Input && !IsExec(P) && P->PinName != UEdGraphSchema_K2::PN_Self)
					{
						Value = Expr(P, Depth + 1);
						if (Value.IsEmpty())
						{
							Value = P->DefaultValue.IsEmpty() ? EmptyValue(P->PinType) : Clean(P->DefaultValue, 160);
						}
						break;
					}
				}
				return Cap(TargetPrefix(N, Depth) + TEXT("Set ") + VS->GetVarNameString() + TEXT(" = ") + Value, MaxExprLen);
			}
			if (const UK2Node* K = Cast<UK2Node>(N))
			{
				if (K->ShouldDrawCompact())
				{
					const FString CT = OneLine(K->GetCompactNodeTitle().ToString());
					TArray<FString> Vals;
					for (const UEdGraphPin* P : N->Pins)
					{
						if (!Visible(P) || P->Direction != EGPD_Input || IsExec(P) || P->PinName == UEdGraphSchema_K2::PN_Self)
						{
							continue;
						}
						FString V = Expr(P, Depth + 1);
						if (V.IsEmpty())
						{
							if (Vals.Num() >= 2)
							{
								continue;  // extra operands left empty (e.g. an error tolerance)
							}
							V = !P->DefaultValue.IsEmpty() ? Clean(P->DefaultValue, 80) : EmptyValue(P->PinType);
						}
						Vals.Add(V);
					}
					bool bSymbolic = !CT.IsEmpty();
					for (TCHAR Ch : CT)
					{
						if (FChar::IsAlnum(Ch))
						{
							bSymbolic = false;
						}
					}
					static const TSet<FString> LogicWords = { TEXT("AND"), TEXT("OR"), TEXT("XOR"), TEXT("NOT"), TEXT("NAND"), TEXT("NOR") };
					bSymbolic = bSymbolic || LogicWords.Contains(CT);
					const bool bConversion = CT.IsEmpty() || CT == TEXT("->") || CT == TEXT("•");
					if (bConversion && Vals.Num() == 1)
					{
						return Vals[0];
					}
					if (bSymbolic && Vals.Num() == 1)
					{
						return Cap(CT + TEXT(" ") + Vals[0], MaxExprLen);
					}
					static const TSet<FString> Comparisons = { TEXT("=="), TEXT("!="), TEXT("<"), TEXT(">"), TEXT("<="), TEXT(">=") };
					if (bSymbolic && Vals.Num() >= 3 && Comparisons.Contains(CT))
					{
						return Cap(TEXT("(") + Vals[0] + TEXT(" ") + CT + TEXT(" ") + Vals[1] + TEXT(" within ") + Vals[2] + TEXT(")"), MaxExprLen);
					}
					if (bSymbolic && Vals.Num() >= 2)
					{
						return Cap(TEXT("(") + FString::Join(Vals, *(TEXT(" ") + CT + TEXT(" "))) + TEXT(")"), MaxExprLen);
					}
				}
			}
			TArray<FString> Args;
			for (const UEdGraphPin* P : N->Pins)
			{
				if (!Visible(P) || P->Direction != EGPD_Input || IsExec(P) || P->PinName == UEdGraphSchema_K2::PN_Self)
				{
					continue;
				}
				const FString V = Expr(P, Depth + 1);
				if (V.IsEmpty())
				{
					continue;
				}
				const FString Name = PinLabel(N, P);
				if (bPure && Name.Len() <= 1)
				{
					Args.Add(V);
				}
				else
				{
					Args.Add(Name + TEXT(" = ") + V);
				}
			}
			return Cap(TargetPrefix(N, Depth) + Title(N) + TEXT("(") + FString::Join(Args, TEXT(", ")) + TEXT(")"), MaxExprLen);
		}
	};

	// ---------------------------------------------------------------- Blueprints

	void CollectSubGraphs(UEdGraph* G, TArray<TPair<UEdGraph*, FString>>& Result, int32 Depth)
	{
		if (!G || Depth > 6)
		{
			return;
		}
		for (UEdGraph* Sub : G->SubGraphs)
		{
			if (Sub)
			{
				Result.Add(TPair<UEdGraph*, FString>(Sub, G->GetName()));
				CollectSubGraphs(Sub, Result, Depth + 1);
			}
		}
	}

	void ExportComponentTree(FOut& Out, USCS_Node* Node, int32 Indent, int32 Depth)
	{
		if (!Node || Depth > 20)
		{
			return;
		}
		UActorComponent* T = Node->ComponentTemplate;
		UClass* C = Node->ComponentClass;
		FString Line = Node->GetVariableName().ToString() + TEXT(" (") + (C ? C->GetName() : FString(TEXT("?"))) + TEXT(")");
		if (Depth == 0 && Node->ParentComponentOrVariableName != NAME_None)
		{
			Line += TEXT(" attached to ") + Node->ParentComponentOrVariableName.ToString();
		}
		if (Node->AttachToName != NAME_None)
		{
			Line += TEXT(" at socket ") + Node->AttachToName.ToString();
		}
		TArray<FString> Props;
		if (T && C)
		{
			FDiffOptions O;
			O.Max = MaxComponentProps;
			O.MaxLen = 160;
			O.SkipLongerThan = 400;
			DiffProps(T, C->GetDefaultObject(), Props, O);
		}
		const FString Joined = JoinProps(Props);
		if (Joined.Len() + Line.Len() < 160)
		{
			Out.Add(Indent, Props.Num() ? Line + TEXT(": ") + Joined : Line);
		}
		else
		{
			Out.Add(Indent, Line + TEXT(":"));
			for (const FString& P : Props)
			{
				Out.Add(Indent + 2, P);
			}
		}
		for (USCS_Node* Child : Node->GetChildNodes())
		{
			ExportComponentTree(Out, Child, Indent + 1, Depth + 1);
		}
	}

	void ExportWidget(FOut& Out, UWidget* W, int32 Indent, int32 Depth)
	{
		if (!W || Depth > 30 || Out.Full())
		{
			return;
		}
		FString Line = W->GetName() + TEXT(" (") + W->GetClass()->GetName() + TEXT(")");
		TArray<FString> Props;
		FDiffOptions O;
		O.Max = MaxWidgetProps;
		O.MaxLen = 100;
		O.bSimpleOnly = true;
		DiffProps(W, W->GetClass()->GetDefaultObject(), Props, O);
		if (Props.Num() > 0)
		{
			Line += TEXT(": ") + JoinProps(Props);
		}
		Out.Add(Indent, Line);
		if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
		{
			for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
			{
				ExportWidget(Out, Panel->GetChildAt(i), Indent + 1, Depth + 1);
			}
		}
	}

	FString VarFlags(const FBPVariableDescription& V)
	{
		TArray<FString> F;
		if ((V.PropertyFlags & CPF_Edit) && !(V.PropertyFlags & CPF_DisableEditOnInstance))
		{
			F.Add(TEXT("instance editable"));
		}
		if (V.HasMetaData(FBlueprintMetadata::MD_ExposeOnSpawn))
		{
			F.Add(TEXT("expose on spawn"));
		}
		if (V.PropertyFlags & CPF_Net)
		{
			F.Add(V.RepNotifyFunc != NAME_None ? TEXT("replicated, notify ") + V.RepNotifyFunc.ToString() : FString(TEXT("replicated")));
		}
		if (V.PropertyFlags & CPF_BlueprintReadOnly)
		{
			F.Add(TEXT("read only"));
		}
		if (V.HasMetaData(FBlueprintMetadata::MD_Private) && V.GetMetaData(FBlueprintMetadata::MD_Private) == TEXT("true"))
		{
			F.Add(TEXT("private"));
		}
		return F.Num() ? TEXT(" [") + FString::Join(F, TEXT(", ")) + TEXT("]") : FString();
	}

	FString ExportBlueprint(UBlueprint* BP, const FString& DisplayPath)
	{
		FOut Out;
		UClass* Gen = BP->GeneratedClass;
		UObject* CDO = Gen ? Gen->GetDefaultObject() : nullptr;

		FString Type = StaticEnum<EBlueprintType>() ? StaticEnum<EBlueprintType>()->GetNameStringByValue((int64)BP->BlueprintType) : FString();
		Type.RemoveFromStart(TEXT("BPTYPE_"));
		Out.Add(0, TEXT("Blueprint ") + DisplayPath + TEXT("  (") + BP->GetClass()->GetName() + TEXT(", type ") + Type + TEXT(")"));
		Out.Add(0, TEXT("Parent: ") + (BP->ParentClass ? BP->ParentClass->GetPathName() : FString(TEXT("none"))));
		Out.Add(0, TEXT("Generated class: ") + (Gen ? Gen->GetPathName() : FString(TEXT("none (not compiled)"))));
		if (!BP->BlueprintDescription.IsEmpty())
		{
			Out.Add(0, TEXT("Description: ") + Cap(OneLine(BP->BlueprintDescription), 400));
		}
		if (BP->ImplementedInterfaces.Num() > 0)
		{
			TArray<FString> I;
			for (const FBPInterfaceDescription& D : BP->ImplementedInterfaces)
			{
				I.Add(D.Interface ? D.Interface->GetName() : FString(TEXT("?")));
			}
			Out.Add(0, TEXT("Interfaces: ") + FString::Join(I, TEXT(", ")));
		}

		// Variables
		TArray<const FBPVariableDescription*> Dispatchers;
		bool bVarHeader = false;
		TSet<FName> OwnVars;
		for (const FBPVariableDescription& V : BP->NewVariables)
		{
			OwnVars.Add(V.VarName);
			if (V.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			{
				Dispatchers.Add(&V);
				continue;
			}
			if (!bVarHeader)
			{
				Out.Add(0, TEXT("Variables:"));
				bVarHeader = true;
			}
			FString Line = V.VarName.ToString() + TEXT(": ") + PinTypeStr(V.VarType);
			FString Value;
			const FProperty* P = Gen ? FindFProperty<FProperty>(Gen, V.VarName) : nullptr;
			if (P && CDO)
			{
				Value = ExportValue(P, P->ContainerPtrToValuePtr<void>(CDO));
			}
			else
			{
				Value = Clean(V.DefaultValue);
			}
			if (!Value.IsEmpty() && Value != TEXT("None") && Value != TEXT("()"))
			{
				Line += TEXT(" = ") + Value;
			}
			Line += VarFlags(V);
			const FString Cat = V.Category.ToString();
			if (!Cat.IsEmpty() && Cat != TEXT("Default") && Cat != BP->GetName())
			{
				Line += TEXT(" (category ") + Cat + TEXT(")");
			}
			if (V.HasMetaData(FBlueprintMetadata::MD_Tooltip))
			{
				const FString Tip = OneLine(V.GetMetaData(FBlueprintMetadata::MD_Tooltip));
				if (!Tip.IsEmpty())
				{
					Line += TEXT("  // ") + Cap(Tip, 160);
				}
			}
			Out.Add(1, Line);
		}

		// Event dispatchers
		if (Dispatchers.Num() > 0)
		{
			Out.Add(0, TEXT("Event dispatchers:"));
			for (const FBPVariableDescription* V : Dispatchers)
			{
				TArray<FString> Params;
				const FMulticastDelegateProperty* MP = Gen ? FindFProperty<FMulticastDelegateProperty>(Gen, V->VarName) : nullptr;
				if (MP && MP->SignatureFunction)
				{
					for (TFieldIterator<FProperty> It(MP->SignatureFunction); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
					{
						Params.Add(It->GetAuthoredName() + TEXT(": ") + PropTypeStr(*It));
					}
				}
				Out.Add(1, V->VarName.ToString() + TEXT("(") + FString::Join(Params, TEXT(", ")) + TEXT(")"));
			}
		}

		// Components
		if (BP->SimpleConstructionScript && BP->SimpleConstructionScript->GetRootNodes().Num() > 0)
		{
			Out.Add(0, TEXT("Components:"));
			for (USCS_Node* Root : BP->SimpleConstructionScript->GetRootNodes())
			{
				ExportComponentTree(Out, Root, 1, 0);
			}
		}
		{
			TArray<FString> Inherited;
			if (BP->InheritableComponentHandler)
			{
				TArray<UActorComponent*> Templates;
				BP->InheritableComponentHandler->GetAllTemplates(Templates);
				for (UActorComponent* T : Templates)
				{
					UObject* Arch = T ? T->GetArchetype() : nullptr;
					if (!T || !Arch || Arch == T)
					{
						continue;
					}
					TArray<FString> Props;
					FDiffOptions O;
					O.Max = MaxComponentProps;
					O.MaxLen = 160;
			O.SkipLongerThan = 400;
					DiffProps(T, Arch, Props, O);
					if (Props.Num() > 0)
					{
						FString Name = T->GetName();
						Name.RemoveFromEnd(TEXT("_GEN_VARIABLE"));
						Inherited.Add(Name + TEXT(" (") + T->GetClass()->GetName() + TEXT("): ") + JoinProps(Props));
					}
				}
			}
			if (CDO)
			{
				TArray<UObject*> Subs;
				CDO->GetDefaultSubobjects(Subs);
				for (UObject* Sub : Subs)
				{
					UActorComponent* C = Cast<UActorComponent>(Sub);
					UObject* Arch = C ? C->GetArchetype() : nullptr;
					if (!C || !Arch || Arch == C)
					{
						continue;
					}
					TArray<FString> Props;
					FDiffOptions O;
					O.Max = MaxComponentProps;
					O.MaxLen = 160;
			O.SkipLongerThan = 400;
					DiffProps(C, Arch, Props, O);
					if (Props.Num() > 0)
					{
						Inherited.Add(C->GetName() + TEXT(" (") + C->GetClass()->GetName() + TEXT("): ") + JoinProps(Props));
					}
				}
			}
			if (Inherited.Num() > 0)
			{
				Out.Add(0, TEXT("Inherited components changed:"));
				for (const FString& S : Inherited)
				{
					Out.Add(1, S);
				}
			}
		}

		// Class defaults changed from parent
		if (CDO && Gen->GetSuperClass())
		{
			TArray<FString> Props;
			FDiffOptions O;
			O.Max = MaxClassDefaultProps;
			O.bSkipOwnClass = true;
			O.Skip = &OwnVars;
			DiffProps(CDO, Gen->GetSuperClass()->GetDefaultObject(), Props, O);
			if (Props.Num() > 0)
			{
				Out.Add(0, TEXT("Class defaults changed from parent:"));
				for (const FString& S : Props)
				{
					Out.Add(1, S);
				}
			}
		}

		// Timelines
		if (BP->Timelines.Num() > 0)
		{
			Out.Add(0, TEXT("Timelines:"));
			for (const UTimelineTemplate* T : BP->Timelines)
			{
				if (!T)
				{
					continue;
				}
				FString Line = T->GetVariableName().ToString() + FString::Printf(TEXT(": length %s"), *TrimFloats(FString::Printf(TEXT("%f"), T->TimelineLength)));
				if (T->bLoop) Line += TEXT(", looping");
				if (T->bAutoPlay) Line += TEXT(", autoplay");
				TArray<FString> Tracks;
				for (const FTTFloatTrack& Tr : T->FloatTracks)
				{
					FString S = TEXT("float ") + Tr.GetTrackName().ToString();
					if (Tr.CurveFloat)
					{
						TArray<FString> Keys;
						for (const FRichCurveKey& K : Tr.CurveFloat->FloatCurve.Keys)
						{
							if (Keys.Num() >= 6)
							{
								Keys.Add(TEXT("..."));
								break;
							}
							Keys.Add(TrimFloats(FString::Printf(TEXT("(%f, %f)"), K.Time, K.Value)));
						}
						S += TEXT(" keys ") + FString::Join(Keys, TEXT(" "));
					}
					Tracks.Add(S);
				}
				for (const FTTEventTrack& Tr : T->EventTracks)
				{
					Tracks.Add(TEXT("event ") + Tr.GetTrackName().ToString());
				}
				for (const FTTVectorTrack& Tr : T->VectorTracks)
				{
					Tracks.Add(TEXT("vector ") + Tr.GetTrackName().ToString());
				}
				for (const FTTLinearColorTrack& Tr : T->LinearColorTracks)
				{
					Tracks.Add(TEXT("color ") + Tr.GetTrackName().ToString());
				}
				if (Tracks.Num() > 0)
				{
					Line += TEXT("; tracks: ") + FString::Join(Tracks, TEXT("; "));
				}
				Out.Add(1, Line);
			}
		}

		// Widget Blueprints
		if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP))
		{
			if (WBP->WidgetTree && WBP->WidgetTree->RootWidget)
			{
				Out.Add(0, TEXT("Widget tree:"));
				ExportWidget(Out, WBP->WidgetTree->RootWidget, 1, 0);
			}
			if (WBP->Bindings.Num() > 0)
			{
				Out.Add(0, TEXT("Property bindings:"));
				for (const FDelegateEditorBinding& B : WBP->Bindings)
				{
					FString Src = B.FunctionName != NAME_None ? B.FunctionName.ToString() + TEXT("()") : B.SourceProperty.ToString();
					Out.Add(1, B.ObjectName + TEXT(".") + B.PropertyName.ToString() + TEXT(" <- ") + Src);
				}
			}
			if (WBP->Animations.Num() > 0)
			{
				TArray<FString> A;
				for (const UWidgetAnimation* Anim : WBP->Animations)
				{
					if (Anim)
					{
						A.Add(Anim->GetName());
					}
				}
				Out.Add(0, TEXT("Widget animations: ") + FString::Join(A, TEXT(", ")));
			}
		}

		// Graphs
		TArray<TPair<UEdGraph*, FString>> Collapsed;
		const bool bKeepEmpty = BP->BlueprintType == BPTYPE_Interface;  // an interface's graphs are its signatures
		TArray<FString> Skipped;
		auto IsK2 = [](const UEdGraph* G) { return G->GetSchema() && G->GetSchema()->IsA<UEdGraphSchema_K2>(); };
		auto RenderOne = [&Out, &Collapsed, &Skipped, &IsK2, bKeepEmpty](UEdGraph* G, const FString& Kind, const FString& Suffix, int32 Max)
		{
			if (!G || Out.Full())
			{
				return;
			}
			if (!IsK2(G))
			{
				// Rig / material / state machine layouts: not Blueprint logic.
				Skipped.Add(G->GetName());
				CollectSubGraphs(G, Collapsed, 0);
				return;
			}
			const int32 Before = Out.Lines.Num();
			Out.Add(0, FString());
			FGraphRenderer R(Out, Max);
			R.Render(G, Kind, Suffix);
			if (!R.HasBody() && !bKeepEmpty)
			{
				Out.Lines.SetNum(Before);
			}
			CollectSubGraphs(G, Collapsed, 0);
		};
		for (UEdGraph* G : BP->UbergraphPages)
		{
			RenderOne(G, TEXT("Graph"), FString(), MaxGraphLines);
		}
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			const bool bExec = G && G->GetSchema() && G->GetSchema()->IsA<UEdGraphSchema_K2>();
			RenderOne(G, bExec ? TEXT("Function") : TEXT("Graph"), FString(), bExec ? MaxGraphLines : MaxDataGraphLines);
		}
		for (const FBPInterfaceDescription& D : BP->ImplementedInterfaces)
		{
			for (UEdGraph* G : D.Graphs)
			{
				RenderOne(G, TEXT("Interface function"), D.Interface ? TEXT(" (") + D.Interface->GetName() + TEXT(")") : FString(), MaxGraphLines);
			}
		}
		for (UEdGraph* G : BP->MacroGraphs)
		{
			RenderOne(G, TEXT("Macro"), FString(), MaxGraphLines);
		}
		for (int32 i = 0; i < Collapsed.Num() && !Out.Full(); ++i)
		{
			UEdGraph* G = Collapsed[i].Key;
			if (!IsK2(G))
			{
				Skipped.Add(G->GetName());
				continue;
			}
			const bool bExec = G->GetSchema()->GetClass() == UEdGraphSchema_K2::StaticClass();
			const int32 Before = Out.Lines.Num();
			Out.Add(0, FString());
			FGraphRenderer R(Out, bExec ? MaxGraphLines : MaxDataGraphLines);
			R.Render(G, TEXT("Collapsed graph"), TEXT(" (in ") + Collapsed[i].Value + TEXT(")"));
			if (!R.HasBody())
			{
				Out.Lines.SetNum(Before);
			}
		}
		if (Skipped.Num() > 0)
		{
			Out.Add(0, FString());
			Out.Add(0, FString::Printf(TEXT("Not rendered (%d non-Blueprint-logic graphs): "), Skipped.Num()) + Cap(FString::Join(Skipped, TEXT(", ")), 600));
		}
		return Out.Join();
	}
}
