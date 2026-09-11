#include "UMGAutoBuilderService.h"

#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/SpinBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Internationalization/Text.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace GG_UMGAutoBuilder
{
	static void AddWarning(FUMGAutoBuilderReport& Report, const FString& Message)
	{
		Report.Warnings.Add(Message);
	}

	static void AddError(FUMGAutoBuilderReport& Report, const FString& Message)
	{
		Report.Errors.Add(Message);
	}

	static void WarnUnknownKeys(const TSharedPtr<FJsonObject>& Obj, const TSet<FString>& AllowedKeys, const FString& Context, FUMGAutoBuilderReport& Report)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (!AllowedKeys.Contains(Pair.Key))
			{
				AddWarning(Report, FString::Printf(TEXT("%s 存在未识别字段：%s"), *Context, *Pair.Key));
			}
		}
	}

	static EHorizontalAlignment ParseHAlign(const FString& S, const EHorizontalAlignment DefaultValue)
	{
		if (S.Equals(TEXT("Fill"), ESearchCase::IgnoreCase)) return HAlign_Fill;
		if (S.Equals(TEXT("Left"), ESearchCase::IgnoreCase)) return HAlign_Left;
		if (S.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return HAlign_Center;
		if (S.Equals(TEXT("Right"), ESearchCase::IgnoreCase)) return HAlign_Right;
		return DefaultValue;
	}

	static EVerticalAlignment ParseVAlign(const FString& S, const EVerticalAlignment DefaultValue)
	{
		if (S.Equals(TEXT("Fill"), ESearchCase::IgnoreCase)) return VAlign_Fill;
		if (S.Equals(TEXT("Top"), ESearchCase::IgnoreCase)) return VAlign_Top;
		if (S.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return VAlign_Center;
		if (S.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) return VAlign_Bottom;
		return DefaultValue;
	}

	static EOrientation ParseOrientation(const FString& S, const EOrientation DefaultValue)
	{
		if (S.Equals(TEXT("Horizontal"), ESearchCase::IgnoreCase)) return Orient_Horizontal;
		if (S.Equals(TEXT("Vertical"), ESearchCase::IgnoreCase)) return Orient_Vertical;
		return DefaultValue;
	}

	static FString ToHAlignString(const EHorizontalAlignment Value)
	{
		switch (Value)
		{
		case HAlign_Fill: return TEXT("Fill");
		case HAlign_Left: return TEXT("Left");
		case HAlign_Center: return TEXT("Center");
		case HAlign_Right: return TEXT("Right");
		default: return TEXT("Fill");
		}
	}

	static FString ToVAlignString(const EVerticalAlignment Value)
	{
		switch (Value)
		{
		case VAlign_Fill: return TEXT("Fill");
		case VAlign_Top: return TEXT("Top");
		case VAlign_Center: return TEXT("Center");
		case VAlign_Bottom: return TEXT("Bottom");
		default: return TEXT("Fill");
		}
	}

	static FString ToOrientationString(const EOrientation Value)
	{
		return Value == Orient_Vertical ? TEXT("Vertical") : TEXT("Horizontal");
	}

	static FString ToSizeRuleString(const FSlateChildSize& Size)
	{
		return Size.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Auto");
	}

	static void ApplySizeRuleIfProvided(const TSharedPtr<FJsonObject>& SlotObj, FSlateChildSize& InOutSize)
	{
		if (!SlotObj.IsValid()) return;

		FString Rule;
		if (SlotObj->TryGetStringField(TEXT("sizeRule"), Rule))
		{
			if (Rule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
			{
				InOutSize.SizeRule = ESlateSizeRule::Fill;
			}
			else if (Rule.Equals(TEXT("Auto"), ESearchCase::IgnoreCase))
			{
				InOutSize.SizeRule = ESlateSizeRule::Automatic;
			}
		}

		double Fill = 0.0;
		if (SlotObj->TryGetNumberField(TEXT("fill"), Fill))
		{
			InOutSize.Value = static_cast<float>(Fill);
		}
	}

	static bool LoadJsonObjectFromFile(const FString& InPath, TSharedPtr<FJsonObject>& OutObj, FUMGAutoBuilderReport& Report)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *InPath))
		{
			AddError(Report, FString::Printf(TEXT("无法读取配置文件：%s"), *InPath));
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(Report, FString::Printf(TEXT("JSON 解析失败：%s"), *InPath));
			return false;
		}
		return true;
	}

	static UClass* ResolveWidgetClassByType(const FString& Type)
	{
		if (Type.Equals(TEXT("CanvasPanel"), ESearchCase::IgnoreCase)) return UCanvasPanel::StaticClass();
		if (Type.Equals(TEXT("Overlay"), ESearchCase::IgnoreCase)) return UOverlay::StaticClass();
		if (Type.Equals(TEXT("VerticalBox"), ESearchCase::IgnoreCase)) return UVerticalBox::StaticClass();
		if (Type.Equals(TEXT("HorizontalBox"), ESearchCase::IgnoreCase)) return UHorizontalBox::StaticClass();
		if (Type.Equals(TEXT("WrapBox"), ESearchCase::IgnoreCase)) return UWrapBox::StaticClass();
		if (Type.Equals(TEXT("Border"), ESearchCase::IgnoreCase)) return UBorder::StaticClass();
		if (Type.Equals(TEXT("Button"), ESearchCase::IgnoreCase)) return UButton::StaticClass();
		if (Type.Equals(TEXT("SizeBox"), ESearchCase::IgnoreCase)) return USizeBox::StaticClass();
		if (Type.Equals(TEXT("Image"), ESearchCase::IgnoreCase)) return UImage::StaticClass();
		if (Type.Equals(TEXT("TextBlock"), ESearchCase::IgnoreCase)) return UTextBlock::StaticClass();
		if (Type.Equals(TEXT("ProgressBar"), ESearchCase::IgnoreCase)) return UProgressBar::StaticClass();
		if (Type.Equals(TEXT("ScrollBox"), ESearchCase::IgnoreCase)) return UScrollBox::StaticClass();
		if (Type.Equals(TEXT("Slider"), ESearchCase::IgnoreCase)) return USlider::StaticClass();
		if (Type.Equals(TEXT("SpinBox"), ESearchCase::IgnoreCase)) return USpinBox::StaticClass();
		if (Type.Equals(TEXT("CheckBox"), ESearchCase::IgnoreCase)) return UCheckBox::StaticClass();
		if (Type.Equals(TEXT("ComboBoxString"), ESearchCase::IgnoreCase)) return UComboBoxString::StaticClass();
		if (Type.Equals(TEXT("Spacer"), ESearchCase::IgnoreCase)) return USpacer::StaticClass();
		return nullptr;
	}

	static FVector2D ReadVec2(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const FVector2D& DefaultValue)
	{
		if (!Obj.IsValid()) return DefaultValue;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 2) return DefaultValue;
		return FVector2D(static_cast<float>((*Arr)[0]->AsNumber()), static_cast<float>((*Arr)[1]->AsNumber()));
	}

	static FAnchors ReadAnchors(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const FAnchors& DefaultValue)
	{
		if (!Obj.IsValid()) return DefaultValue;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 4) return DefaultValue;
		return FAnchors(
			static_cast<float>((*Arr)[0]->AsNumber()),
			static_cast<float>((*Arr)[1]->AsNumber()),
			static_cast<float>((*Arr)[2]->AsNumber()),
			static_cast<float>((*Arr)[3]->AsNumber()));
	}

	static FMargin ReadMargin(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const FMargin& DefaultValue)
	{
		if (!Obj.IsValid()) return DefaultValue;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 4) return DefaultValue;
		return FMargin(
			static_cast<float>((*Arr)[0]->AsNumber()),
			static_cast<float>((*Arr)[1]->AsNumber()),
			static_cast<float>((*Arr)[2]->AsNumber()),
			static_cast<float>((*Arr)[3]->AsNumber()));
	}

	static FLinearColor ReadColor(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const FLinearColor& DefaultValue)
	{
		if (!Obj.IsValid()) return DefaultValue;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Key, Arr) || !Arr || Arr->Num() < 4) return DefaultValue;
		return FLinearColor(
			static_cast<float>((*Arr)[0]->AsNumber()),
			static_cast<float>((*Arr)[1]->AsNumber()),
			static_cast<float>((*Arr)[2]->AsNumber()),
			static_cast<float>((*Arr)[3]->AsNumber()));
	}

	static UTexture2D* TryLoadTexture(const FString& TextureObjectPath)
	{
		if (TextureObjectPath.IsEmpty())
		{
			return nullptr;
		}

		UObject* Obj = FSoftObjectPath(TextureObjectPath).TryLoad();
		return Cast<UTexture2D>(Obj);
	}

	static void ApplyCommonWidgetFlags(UWidget* Widget, const TSharedPtr<FJsonObject>& PropsObj)
	{
		if (!Widget || !PropsObj.IsValid()) return;

		bool bIsVariable = false;
		if (PropsObj->TryGetBoolField(TEXT("isVariable"), bIsVariable))
		{
			Widget->bIsVariable = bIsVariable;
		}
	}

	static void ApplyWidgetProperties(UWidget* Widget, const TSharedPtr<FJsonObject>& PropsObj)
	{
		if (!Widget || !PropsObj.IsValid()) return;

		ApplyCommonWidgetFlags(Widget, PropsObj);

		bool bEnabled = true;
		if (PropsObj->TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			Widget->SetIsEnabled(bEnabled);
		}

		if (UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			FString S;
			if (PropsObj->TryGetStringField(TEXT("text"), S))
			{
				Text->SetText(FText::FromString(S));
			}

			double FontSize = 0.0;
			if (PropsObj->TryGetNumberField(TEXT("fontSize"), FontSize))
			{
				FSlateFontInfo Font = Text->GetFont();
				Font.Size = static_cast<int32>(FontSize);
				Text->SetFont(Font);
			}

			if (PropsObj->HasField(TEXT("color")))
			{
				Text->SetColorAndOpacity(FSlateColor(ReadColor(PropsObj, TEXT("color"), FLinearColor::White)));
			}
		}
		else if (UImage* Image = Cast<UImage>(Widget))
		{
			FString TexturePath;
			if (PropsObj->TryGetStringField(TEXT("texture"), TexturePath))
			{
				if (UTexture2D* Tex = TryLoadTexture(TexturePath))
				{
					FSlateBrush Brush = Image->GetBrush();
					Brush.SetResourceObject(Tex);
					Image->SetBrush(Brush);
				}
			}

			if (PropsObj->HasField(TEXT("color")))
			{
				Image->SetColorAndOpacity(ReadColor(PropsObj, TEXT("color"), Image->GetColorAndOpacity()));
			}
		}
		else if (UProgressBar* PB = Cast<UProgressBar>(Widget))
		{
			double Percent = -1.0;
			if (PropsObj->TryGetNumberField(TEXT("percent"), Percent))
			{
				PB->SetPercent(static_cast<float>(Percent));
			}

			if (PropsObj->HasField(TEXT("fillColor")))
			{
				PB->SetFillColorAndOpacity(ReadColor(PropsObj, TEXT("fillColor"), FLinearColor::White));
			}
		}
		else if (UButton* Button = Cast<UButton>(Widget))
		{
			if (PropsObj->HasField(TEXT("backgroundColor")))
			{
				Button->SetBackgroundColor(ReadColor(PropsObj, TEXT("backgroundColor"), FLinearColor::White));
			}
			if (PropsObj->HasField(TEXT("contentColor")))
			{
				Button->SetColorAndOpacity(ReadColor(PropsObj, TEXT("contentColor"), FLinearColor::White));
			}
		}
		else if (USlider* Slider = Cast<USlider>(Widget))
		{
			double Number = 0.0;
			if (PropsObj->TryGetNumberField(TEXT("minValue"), Number)) Slider->SetMinValue(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("maxValue"), Number)) Slider->SetMaxValue(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("value"), Number)) Slider->SetValue(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("stepSize"), Number)) Slider->SetStepSize(static_cast<float>(Number));
			if (PropsObj->HasField(TEXT("barColor")))
			{
				Slider->SetSliderBarColor(ReadColor(PropsObj, TEXT("barColor"), FLinearColor::White));
			}
			if (PropsObj->HasField(TEXT("handleColor")))
			{
				Slider->SetSliderHandleColor(ReadColor(PropsObj, TEXT("handleColor"), FLinearColor::White));
			}
		}
		else if (USpinBox* SpinBox = Cast<USpinBox>(Widget))
		{
			double Number = 0.0;
			if (PropsObj->TryGetNumberField(TEXT("minValue"), Number)) SpinBox->SetMinValue(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("maxValue"), Number)) SpinBox->SetMaxValue(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("minSliderValue"), Number)) SpinBox->SetMinSliderValue(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("maxSliderValue"), Number)) SpinBox->SetMaxSliderValue(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("value"), Number)) SpinBox->SetValue(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("delta"), Number)) SpinBox->SetDelta(static_cast<float>(Number));
			if (PropsObj->TryGetNumberField(TEXT("minFractionalDigits"), Number)) SpinBox->SetMinFractionalDigits(static_cast<int32>(Number));
			if (PropsObj->TryGetNumberField(TEXT("maxFractionalDigits"), Number)) SpinBox->SetMaxFractionalDigits(static_cast<int32>(Number));
			if (PropsObj->TryGetNumberField(TEXT("minDesiredWidth"), Number)) SpinBox->SetMinDesiredWidth(static_cast<float>(Number));
			if (PropsObj->HasField(TEXT("foregroundColor")))
			{
				SpinBox->SetForegroundColor(FSlateColor(ReadColor(PropsObj, TEXT("foregroundColor"), FLinearColor::White)));
			}
		}
		else if (UCheckBox* CheckBox = Cast<UCheckBox>(Widget))
		{
			bool bChecked = false;
			if (PropsObj->TryGetBoolField(TEXT("checked"), bChecked))
			{
				CheckBox->SetIsChecked(bChecked);
			}
		}
		else if (UComboBoxString* ComboBox = Cast<UComboBoxString>(Widget))
		{
			const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
			if (PropsObj->TryGetArrayField(TEXT("options"), Options) && Options)
			{
				ComboBox->ClearOptions();
				for (const TSharedPtr<FJsonValue>& Option : *Options)
				{
					if (Option.IsValid()) ComboBox->AddOption(Option->AsString());
				}
			}

			FString SelectedOption;
			if (PropsObj->TryGetStringField(TEXT("selectedOption"), SelectedOption))
			{
				ComboBox->SetSelectedOption(SelectedOption);
			}
		}
		else if (UScrollBox* ScrollBox = Cast<UScrollBox>(Widget))
		{
			bool bValue = false;
			if (PropsObj->TryGetBoolField(TEXT("alwaysShowScrollbar"), bValue)) ScrollBox->SetAlwaysShowScrollbar(bValue);
			if (PropsObj->TryGetBoolField(TEXT("allowOverscroll"), bValue)) ScrollBox->SetAllowOverscroll(bValue);
			if (PropsObj->TryGetBoolField(TEXT("animateWheelScrolling"), bValue)) ScrollBox->SetAnimateWheelScrolling(bValue);
			double Number = 0.0;
			if (PropsObj->TryGetNumberField(TEXT("wheelScrollMultiplier"), Number)) ScrollBox->SetWheelScrollMultiplier(static_cast<float>(Number));
		}
		else if (UWrapBox* Wrap = Cast<UWrapBox>(Widget))
		{
			if (PropsObj->HasField(TEXT("innerSlotPadding")))
			{
				Wrap->SetInnerSlotPadding(ReadVec2(PropsObj, TEXT("innerSlotPadding"), FVector2D::ZeroVector));
			}

			double WrapSize = 0.0;
			if (PropsObj->TryGetNumberField(TEXT("wrapSize"), WrapSize))
			{
				Wrap->SetWrapSize(static_cast<float>(WrapSize));
			}

			bool bExplicitWrap = false;
			if (PropsObj->TryGetBoolField(TEXT("explicitWrapSize"), bExplicitWrap))
			{
				Wrap->SetExplicitWrapSize(bExplicitWrap);
			}

			FString HorizontalAlignment;
			if (PropsObj->TryGetStringField(TEXT("horizontalAlignment"), HorizontalAlignment))
			{
				Wrap->SetHorizontalAlignment(ParseHAlign(HorizontalAlignment, Wrap->GetHorizontalAlignment()));
			}

			FString Orientation;
			if (PropsObj->TryGetStringField(TEXT("orientation"), Orientation))
			{
				Wrap->SetOrientation(ParseOrientation(Orientation, Wrap->GetOrientation()));
			}
		}
		else if (USizeBox* SB = Cast<USizeBox>(Widget))
		{
			double W = 0.0;
			if (PropsObj->TryGetNumberField(TEXT("widthOverride"), W))
			{
				SB->SetWidthOverride(static_cast<float>(W));
			}

			double H = 0.0;
			if (PropsObj->TryGetNumberField(TEXT("heightOverride"), H))
			{
				SB->SetHeightOverride(static_cast<float>(H));
			}
		}
		else if (UBorder* Border = Cast<UBorder>(Widget))
		{
			if (PropsObj->HasField(TEXT("padding")))
			{
				Border->SetPadding(ReadMargin(PropsObj, TEXT("padding"), Border->GetPadding()));
			}

			if (PropsObj->HasField(TEXT("brushColor")))
			{
				Border->SetBrushColor(ReadColor(PropsObj, TEXT("brushColor"), Border->GetBrushColor()));
			}

			FString TexturePath;
			if (PropsObj->TryGetStringField(TEXT("texture"), TexturePath))
			{
				if (UTexture2D* Tex = TryLoadTexture(TexturePath))
				{
					Border->SetBrushFromTexture(Tex);
				}
			}
		}
		else if (USpacer* Spacer = Cast<USpacer>(Widget))
		{
			if (PropsObj->HasField(TEXT("size")))
			{
				Spacer->SetSize(ReadVec2(PropsObj, TEXT("size"), Spacer->GetSize()));
			}
		}
	}

	static void ApplySlotProperties(UPanelSlot* Slot, const TSharedPtr<FJsonObject>& SlotObj)
	{
		if (!Slot || !SlotObj.IsValid()) return;

		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			CanvasSlot->SetAnchors(ReadAnchors(SlotObj, TEXT("anchors"), CanvasSlot->GetAnchors()));
			CanvasSlot->SetOffsets(ReadMargin(SlotObj, TEXT("offsets"), CanvasSlot->GetOffsets()));
			CanvasSlot->SetAlignment(ReadVec2(SlotObj, TEXT("alignment"), CanvasSlot->GetAlignment()));

			bool bAutoSize = false;
			if (SlotObj->TryGetBoolField(TEXT("autoSize"), bAutoSize))
			{
				CanvasSlot->SetAutoSize(bAutoSize);
			}

			double Z = 0.0;
			if (SlotObj->TryGetNumberField(TEXT("zOrder"), Z))
			{
				CanvasSlot->SetZOrder(static_cast<int32>(Z));
			}
		}
		else if (UHorizontalBoxSlot* HSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			HSlot->SetPadding(ReadMargin(SlotObj, TEXT("padding"), HSlot->GetPadding()));

			if (SlotObj->HasTypedField<EJson::String>(TEXT("hAlign")))
			{
				FString S;
				SlotObj->TryGetStringField(TEXT("hAlign"), S);
				HSlot->SetHorizontalAlignment(ParseHAlign(S, HSlot->GetHorizontalAlignment()));
			}

			if (SlotObj->HasTypedField<EJson::String>(TEXT("vAlign")))
			{
				FString S;
				SlotObj->TryGetStringField(TEXT("vAlign"), S);
				HSlot->SetVerticalAlignment(ParseVAlign(S, HSlot->GetVerticalAlignment()));
			}

			FSlateChildSize Size = HSlot->GetSize();
			ApplySizeRuleIfProvided(SlotObj, Size);
			HSlot->SetSize(Size);
		}
		else if (UVerticalBoxSlot* VSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			VSlot->SetPadding(ReadMargin(SlotObj, TEXT("padding"), VSlot->GetPadding()));

			if (SlotObj->HasTypedField<EJson::String>(TEXT("hAlign")))
			{
				FString S;
				SlotObj->TryGetStringField(TEXT("hAlign"), S);
				VSlot->SetHorizontalAlignment(ParseHAlign(S, VSlot->GetHorizontalAlignment()));
			}

			if (SlotObj->HasTypedField<EJson::String>(TEXT("vAlign")))
			{
				FString S;
				SlotObj->TryGetStringField(TEXT("vAlign"), S);
				VSlot->SetVerticalAlignment(ParseVAlign(S, VSlot->GetVerticalAlignment()));
			}

			FSlateChildSize Size = VSlot->GetSize();
			ApplySizeRuleIfProvided(SlotObj, Size);
			VSlot->SetSize(Size);
		}
		else if (UOverlaySlot* OSlot = Cast<UOverlaySlot>(Slot))
		{
			OSlot->SetPadding(ReadMargin(SlotObj, TEXT("padding"), OSlot->GetPadding()));

			if (SlotObj->HasTypedField<EJson::String>(TEXT("hAlign")))
			{
				FString S;
				SlotObj->TryGetStringField(TEXT("hAlign"), S);
				OSlot->SetHorizontalAlignment(ParseHAlign(S, OSlot->GetHorizontalAlignment()));
			}

			if (SlotObj->HasTypedField<EJson::String>(TEXT("vAlign")))
			{
				FString S;
				SlotObj->TryGetStringField(TEXT("vAlign"), S);
				OSlot->SetVerticalAlignment(ParseVAlign(S, OSlot->GetVerticalAlignment()));
			}
		}
		else if (UWrapBoxSlot* WSlot = Cast<UWrapBoxSlot>(Slot))
		{
			WSlot->SetPadding(ReadMargin(SlotObj, TEXT("padding"), WSlot->GetPadding()));

			if (SlotObj->HasTypedField<EJson::String>(TEXT("hAlign")))
			{
				FString S;
				SlotObj->TryGetStringField(TEXT("hAlign"), S);
				WSlot->SetHorizontalAlignment(ParseHAlign(S, WSlot->GetHorizontalAlignment()));
			}

			if (SlotObj->HasTypedField<EJson::String>(TEXT("vAlign")))
			{
				FString S;
				SlotObj->TryGetStringField(TEXT("vAlign"), S);
				WSlot->SetVerticalAlignment(ParseVAlign(S, WSlot->GetVerticalAlignment()));
			}

			bool bFillEmptySpace = false;
			if (SlotObj->TryGetBoolField(TEXT("fillEmptySpace"), bFillEmptySpace))
			{
				WSlot->SetFillEmptySpace(bFillEmptySpace);
			}

			bool bNewLine = false;
			if (SlotObj->TryGetBoolField(TEXT("newLine"), bNewLine))
			{
				WSlot->SetNewLine(bNewLine);
			}

			double FillSpan = 0.0;
			if (SlotObj->TryGetNumberField(TEXT("fillSpanWhenLessThan"), FillSpan))
			{
				WSlot->SetFillSpanWhenLessThan(static_cast<float>(FillSpan));
			}
		}
	}

	static bool ValidateNodeRecursive(const TSharedPtr<FJsonObject>& NodeObj, const FString& Context, FUMGAutoBuilderReport& Report)
	{
		if (!NodeObj.IsValid())
		{
			AddError(Report, FString::Printf(TEXT("%s 不是有效对象"), *Context));
			return false;
		}

		static const TSet<FString> AllowedNodeKeys = {
			TEXT("type"), TEXT("name"), TEXT("props"), TEXT("slot"), TEXT("children")
		};
		WarnUnknownKeys(NodeObj, AllowedNodeKeys, Context, Report);

		FString Type;
		if (!NodeObj->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
		{
			AddError(Report, FString::Printf(TEXT("%s 缺少 type"), *Context));
			return false;
		}

		if (!Type.Equals(TEXT("UserWidget"), ESearchCase::IgnoreCase) && !ResolveWidgetClassByType(Type))
		{
			AddError(Report, FString::Printf(TEXT("%s 使用了不支持的 type：%s"), *Context, *Type));
			return false;
		}

		if (Type.Equals(TEXT("UserWidget"), ESearchCase::IgnoreCase))
		{
			const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
			if (!NodeObj->TryGetObjectField(TEXT("props"), PropsPtr) || !PropsPtr || !PropsPtr->IsValid())
			{
				AddError(Report, FString::Printf(TEXT("%s 的 UserWidget 缺少 props"), *Context));
				return false;
			}

			FString ClassPath;
			if (!(*PropsPtr)->TryGetStringField(TEXT("class"), ClassPath) || ClassPath.IsEmpty())
			{
				AddError(Report, FString::Printf(TEXT("%s 的 UserWidget 缺少 props.class"), *Context));
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (NodeObj->TryGetArrayField(TEXT("children"), Children) && Children)
		{
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> ChildObj = (*Children)[Index].IsValid() ? (*Children)[Index]->AsObject() : nullptr;
				ValidateNodeRecursive(ChildObj, FString::Printf(TEXT("%s.children[%d]"), *Context, Index), Report);
			}
		}

		return !Report.HasErrors();
	}

	static void ValidateSchemaAndTopLevel(const TSharedPtr<FJsonObject>& RootObj, FUMGAutoBuilderReport& Report)
	{
		static const TSet<FString> AllowedTopLevelKeys = {
			TEXT("schemaVersion"), TEXT("targetWidget"), TEXT("createIfMissing"), TEXT("mode"), TEXT("assertWidgets"), TEXT("root"), TEXT("patch")
		};
		WarnUnknownKeys(RootObj, AllowedTopLevelKeys, TEXT("root"), Report);

		double SchemaVersion = 0.0;
		if (!RootObj->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion))
		{
			AddWarning(Report, FString::Printf(TEXT("配置缺少 schemaVersion，将按 v%d 解析"), FUMGAutoBuilderService::CurrentSchemaVersion));
		}
		else
		{
			const int32 SchemaVersionInt = static_cast<int32>(SchemaVersion);
			if (SchemaVersionInt > FUMGAutoBuilderService::CurrentSchemaVersion)
			{
				AddError(Report, FString::Printf(TEXT("schemaVersion=%d 高于当前支持的版本 %d"), SchemaVersionInt, FUMGAutoBuilderService::CurrentSchemaVersion));
			}
			else if (SchemaVersionInt < FUMGAutoBuilderService::CurrentSchemaVersion)
			{
				AddWarning(Report, FString::Printf(TEXT("schemaVersion=%d 低于当前版本 %d，可能存在兼容差异"), SchemaVersionInt, FUMGAutoBuilderService::CurrentSchemaVersion));
			}
		}
	}

	static bool ValidatePatchSpec(const TSharedPtr<FJsonObject>& RootObj, FUMGAutoBuilderReport& Report)
	{
		const TSharedPtr<FJsonObject>* PatchPtr = nullptr;
		if (!RootObj->TryGetObjectField(TEXT("patch"), PatchPtr) || !PatchPtr || !PatchPtr->IsValid())
		{
			AddError(Report, TEXT("patch 模式需要配置 patch 对象"));
			return false;
		}

		static const TSet<FString> AllowedPatchKeys = { TEXT("setWidgetProps"), TEXT("ensureChildren") };
		const TSharedPtr<FJsonObject> PatchObj = *PatchPtr;
		WarnUnknownKeys(PatchObj, AllowedPatchKeys, TEXT("patch"), Report);

		const TArray<TSharedPtr<FJsonValue>>* SetProps = nullptr;
		if (PatchObj->TryGetArrayField(TEXT("setWidgetProps"), SetProps) && SetProps)
		{
			static const TSet<FString> AllowedSetPropKeys = { TEXT("name"), TEXT("props"), TEXT("slot") };
			for (int32 Index = 0; Index < SetProps->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Item = (*SetProps)[Index].IsValid() ? (*SetProps)[Index]->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					AddError(Report, FString::Printf(TEXT("patch.setWidgetProps[%d] 不是有效对象"), Index));
					continue;
				}

				WarnUnknownKeys(Item, AllowedSetPropKeys, FString::Printf(TEXT("patch.setWidgetProps[%d]"), Index), Report);

				FString Name;
				if (!Item->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
				{
					AddError(Report, FString::Printf(TEXT("patch.setWidgetProps[%d] 缺少 name"), Index));
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* EnsureChildren = nullptr;
		if (PatchObj->TryGetArrayField(TEXT("ensureChildren"), EnsureChildren) && EnsureChildren)
		{
			static const TSet<FString> AllowedEnsureKeys = { TEXT("parent"), TEXT("children") };
			for (int32 Index = 0; Index < EnsureChildren->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Item = (*EnsureChildren)[Index].IsValid() ? (*EnsureChildren)[Index]->AsObject() : nullptr;
				if (!Item.IsValid())
				{
					AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d] 不是有效对象"), Index));
					continue;
				}

				WarnUnknownKeys(Item, AllowedEnsureKeys, FString::Printf(TEXT("patch.ensureChildren[%d]"), Index), Report);

				FString Parent;
				if (!Item->TryGetStringField(TEXT("parent"), Parent) || Parent.IsEmpty())
				{
					AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d] 缺少 parent"), Index));
				}

				const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
				if (!Item->TryGetArrayField(TEXT("children"), Children) || !Children)
				{
					AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d] 缺少 children 数组"), Index));
					continue;
				}

				for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
				{
					const TSharedPtr<FJsonObject> ChildObj = (*Children)[ChildIndex].IsValid() ? (*Children)[ChildIndex]->AsObject() : nullptr;
					ValidateNodeRecursive(ChildObj, FString::Printf(TEXT("patch.ensureChildren[%d].children[%d]"), Index, ChildIndex), Report);
				}
			}
		}

		return !Report.HasErrors();
	}

	static bool ValidateBuildSpec(const TSharedPtr<FJsonObject>& RootObj, FUMGAutoBuilderReport& Report)
	{
		const TSharedPtr<FJsonObject>* RootNodePtr = nullptr;
		if (!RootObj->TryGetObjectField(TEXT("root"), RootNodePtr) || !RootNodePtr || !RootNodePtr->IsValid())
		{
			AddError(Report, TEXT("配置缺少 root 节点"));
			return false;
		}

		return ValidateNodeRecursive(*RootNodePtr, TEXT("root.root"), Report);
	}

	static UWidget* BuildWidgetNode(
		UWidgetBlueprint* WidgetBP,
		UWidgetTree* WidgetTree,
		UPanelWidget* ParentPanel,
		const TSharedPtr<FJsonObject>& NodeObj,
		FUMGAutoBuilderReport& Report,
		const FString& Context)
	{
		if (!WidgetBP || !WidgetTree || !NodeObj.IsValid())
		{
			AddError(Report, FString::Printf(TEXT("%s 构建失败：输入为空"), *Context));
			return nullptr;
		}

		FString Type;
		if (!NodeObj->TryGetStringField(TEXT("type"), Type))
		{
			AddError(Report, FString::Printf(TEXT("%s 缺少 type"), *Context));
			return nullptr;
		}

		FString NameStr;
		NodeObj->TryGetStringField(TEXT("name"), NameStr);
		const FName WidgetName = NameStr.IsEmpty() ? NAME_None : FName(*NameStr);

		UClass* WidgetClass = nullptr;
		if (Type.Equals(TEXT("UserWidget"), ESearchCase::IgnoreCase))
		{
			const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("props"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
			{
				FString ClassPath;
				if ((*PropsPtr)->TryGetStringField(TEXT("class"), ClassPath) && !ClassPath.IsEmpty())
				{
					ClassPath.TrimQuotesInline();
					FSoftClassPath SCP(ClassPath);
					WidgetClass = SCP.TryLoadClass<UUserWidget>();
				}
			}

			if (!WidgetClass)
			{
				AddError(Report, FString::Printf(TEXT("%s 无法加载 props.class：%s"), *Context, *NameStr));
				return nullptr;
			}
		}
		else
		{
			WidgetClass = ResolveWidgetClassByType(Type);
		}

		if (!WidgetClass)
		{
			AddError(Report, FString::Printf(TEXT("%s 使用了不支持的 widget type：%s"), *Context, *Type));
			return nullptr;
		}

		UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetName);
		if (!NewWidget)
		{
			AddError(Report, FString::Printf(TEXT("%s ConstructWidget 失败"), *Context));
			return nullptr;
		}

		if (NodeObj->HasTypedField<EJson::Object>(TEXT("props")))
		{
			const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("props"), PropsPtr) && PropsPtr)
			{
				ApplyWidgetProperties(NewWidget, *PropsPtr);
			}
		}

		if (ParentPanel)
		{
			UPanelSlot* NewSlot = ParentPanel->AddChild(NewWidget);
			if (!NewSlot)
			{
				AddError(Report, FString::Printf(TEXT("%s 添加到父容器失败"), *Context));
				return nullptr;
			}

			if (NodeObj->HasTypedField<EJson::Object>(TEXT("slot")))
			{
				const TSharedPtr<FJsonObject>* SlotPtr = nullptr;
				if (NodeObj->TryGetObjectField(TEXT("slot"), SlotPtr) && SlotPtr)
				{
					ApplySlotProperties(NewSlot, *SlotPtr);
				}
			}
		}

		if (UPanelWidget* AsPanel = Cast<UPanelWidget>(NewWidget))
		{
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("children"), Children) && Children)
			{
				for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
				{
					const TSharedPtr<FJsonObject> ChildObj = (*Children)[ChildIndex].IsValid() ? (*Children)[ChildIndex]->AsObject() : nullptr;
					if (!ChildObj.IsValid())
					{
						AddError(Report, FString::Printf(TEXT("%s.children[%d] 不是有效对象"), *Context, ChildIndex));
						continue;
					}

					BuildWidgetNode(WidgetBP, WidgetTree, AsPanel, ChildObj, Report, FString::Printf(TEXT("%s.children[%d]"), *Context, ChildIndex));
				}
			}
		}

		return NewWidget;
	}

	static bool RebuildWidgetBlueprintFromSpec(UWidgetBlueprint* WidgetBP, const TSharedPtr<FJsonObject>& RootObj, FUMGAutoBuilderReport& Report)
	{
		if (!WidgetBP || !RootObj.IsValid())
		{
			AddError(Report, TEXT("WidgetBlueprint 或 Root 配置为空"));
			return false;
		}

		const TSharedPtr<FJsonObject>* RootNodePtr = nullptr;
		if (!RootObj->TryGetObjectField(TEXT("root"), RootNodePtr) || !RootNodePtr)
		{
			AddError(Report, TEXT("配置缺少 root 节点"));
			return false;
		}
		const TSharedPtr<FJsonObject> RootNode = *RootNodePtr;

		WidgetBP->Modify();

		if (UWidgetTree* OldTree = WidgetBP->WidgetTree.Get())
		{
			OldTree->Modify();
			const FString OldName = OldTree->GetName();
			OldTree->Rename(
				*MakeUniqueObjectName(WidgetBP, UWidgetTree::StaticClass(), FName(*(OldName + TEXT("_OLD")))).ToString(),
				WidgetBP,
				REN_DontCreateRedirectors | REN_NonTransactional);
		}

		UWidgetTree* Tree = NewObject<UWidgetTree>(WidgetBP, TEXT("WidgetTree"), RF_Transactional);
		WidgetBP->WidgetTree = Tree;
		Tree->Modify();

		UWidget* RootWidget = BuildWidgetNode(WidgetBP, Tree, nullptr, RootNode, Report, TEXT("root.root"));
		if (!RootWidget)
		{
			AddError(Report, TEXT("构建 RootWidget 失败"));
			return false;
		}

		Tree->RootWidget = RootWidget;

		// Build 模式会替换整棵 WidgetTree。旧控件变量已不存在，必须同步清除
		// 它们的 GUID；否则 UE 编译器会把旧 GUID 视为已删除但仍被引用的变量。
		WidgetBP->WidgetVariableNameToGuidMap.Reset();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
		WidgetBP->MarkPackageDirty();
		return !Report.HasErrors();
	}

	static bool PatchWidgetBlueprintFromSpec(UWidgetBlueprint* WidgetBP, const TSharedPtr<FJsonObject>& RootObj, FUMGAutoBuilderReport& Report)
	{
		if (!WidgetBP || !RootObj.IsValid())
		{
			AddError(Report, TEXT("WidgetBlueprint 或配置为空"));
			return false;
		}

		const TSharedPtr<FJsonObject>* PatchPtr = nullptr;
		if (!RootObj->TryGetObjectField(TEXT("patch"), PatchPtr) || !PatchPtr || !PatchPtr->IsValid())
		{
			AddError(Report, TEXT("patch 模式需要配置 patch 对象"));
			return false;
		}

		WidgetBP->Modify();

		UWidgetTree* Tree = WidgetBP->WidgetTree.Get();
		if (!Tree || !Tree->RootWidget)
		{
			AddError(Report, TEXT("patch 模式要求目标 WidgetBlueprint 已有有效的 WidgetTree"));
			return false;
		}
		Tree->Modify();

		const TSharedPtr<FJsonObject> PatchObj = *PatchPtr;

		if (PatchObj->HasTypedField<EJson::Array>(TEXT("setWidgetProps")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (PatchObj->TryGetArrayField(TEXT("setWidgetProps"), Arr) && Arr)
			{
				for (int32 Index = 0; Index < Arr->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Item = (*Arr)[Index].IsValid() ? (*Arr)[Index]->AsObject() : nullptr;
					if (!Item.IsValid())
					{
						AddError(Report, FString::Printf(TEXT("patch.setWidgetProps[%d] 不是有效对象"), Index));
						continue;
					}

					FString NameStr;
					if (!Item->TryGetStringField(TEXT("name"), NameStr) || NameStr.IsEmpty())
					{
						AddError(Report, FString::Printf(TEXT("patch.setWidgetProps[%d] 缺少 name"), Index));
						continue;
					}

					UWidget* W = Tree->FindWidget(FName(*NameStr));
					if (!W)
					{
						AddError(Report, FString::Printf(TEXT("patch.setWidgetProps[%d] 找不到控件：%s"), Index, *NameStr));
						continue;
					}

					const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
					if (Item->TryGetObjectField(TEXT("props"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
					{
						ApplyWidgetProperties(W, *PropsPtr);
					}

					const TSharedPtr<FJsonObject>* SlotPtr = nullptr;
					if (Item->TryGetObjectField(TEXT("slot"), SlotPtr) && SlotPtr && SlotPtr->IsValid())
					{
						if (UPanelSlot* Slot = W->Slot)
						{
							ApplySlotProperties(Slot, *SlotPtr);
						}
						else
						{
							AddError(Report, FString::Printf(TEXT("patch.setWidgetProps[%d] 控件 %s 没有 slot，无法应用 slot 属性"), Index, *NameStr));
						}
					}
				}
			}
		}

		if (PatchObj->HasTypedField<EJson::Array>(TEXT("ensureChildren")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (PatchObj->TryGetArrayField(TEXT("ensureChildren"), Arr) && Arr)
			{
				for (int32 Index = 0; Index < Arr->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Item = (*Arr)[Index].IsValid() ? (*Arr)[Index]->AsObject() : nullptr;
					if (!Item.IsValid())
					{
						AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d] 不是有效对象"), Index));
						continue;
					}

					FString ParentName;
					if (!Item->TryGetStringField(TEXT("parent"), ParentName) || ParentName.IsEmpty())
					{
						AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d] 缺少 parent"), Index));
						continue;
					}

					UWidget* ParentW = Tree->FindWidget(FName(*ParentName));
					UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentW);
					if (!ParentPanel)
					{
						AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d] 找不到可追加子控件的父容器：%s"), Index, *ParentName));
						continue;
					}

					const TArray<TSharedPtr<FJsonValue>>* ChildrenArr = nullptr;
					if (!Item->TryGetArrayField(TEXT("children"), ChildrenArr) || !ChildrenArr)
					{
						AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d] 缺少 children 数组"), Index));
						continue;
					}

					for (int32 ChildIndex = 0; ChildIndex < ChildrenArr->Num(); ++ChildIndex)
					{
						const TSharedPtr<FJsonObject> ChildObj = (*ChildrenArr)[ChildIndex].IsValid() ? (*ChildrenArr)[ChildIndex]->AsObject() : nullptr;
						if (!ChildObj.IsValid())
						{
							AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d].children[%d] 不是有效对象"), Index, ChildIndex));
							continue;
						}

						FString ChildName;
						ChildObj->TryGetStringField(TEXT("name"), ChildName);
						if (!ChildName.IsEmpty() && Tree->FindWidget(FName(*ChildName)) != nullptr)
						{
							AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d].children[%d] 控件名已存在：%s"), Index, ChildIndex, *ChildName));
							continue;
						}

						UWidget* ChildWidget = BuildWidgetNode(
							WidgetBP,
							Tree,
							ParentPanel,
							ChildObj,
							Report,
							FString::Printf(TEXT("patch.ensureChildren[%d].children[%d]"), Index, ChildIndex));

						if (!ChildWidget)
						{
							AddError(Report, FString::Printf(TEXT("patch.ensureChildren[%d].children[%d] 构建失败"), Index, ChildIndex));
						}
					}
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
		WidgetBP->MarkPackageDirty();
		return !Report.HasErrors();
	}

	static bool SaveAsset(UObject* Asset, FUMGAutoBuilderReport& Report)
	{
		if (!Asset)
		{
			AddError(Report, TEXT("SaveAsset: Asset 为空"));
			return false;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			AddError(Report, TEXT("SaveAsset: Package 为空"));
			return false;
		}

		const FString PackageName = Package->GetName();
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_None;
		Args.Error = GError;

		const bool bOk = UPackage::SavePackage(Package, Asset, *Filename, Args);
		if (!bOk)
		{
			AddError(Report, FString::Printf(TEXT("保存失败：%s -> %s"), *PackageName, *Filename));
		}
		return bOk;
	}

	static UWidgetBlueprint* LoadWidgetBlueprint(const FString& InPath)
	{
		FString Path = InPath;
		if (Path.EndsWith(TEXT("_C")))
		{
			Path = Path.LeftChop(2);
		}

		UObject* Obj = FSoftObjectPath(Path).TryLoad();
		if (UWidgetBlueprint* WB = Cast<UWidgetBlueprint>(Obj))
		{
			return WB;
		}
		if (UWidgetBlueprintGeneratedClass* WGC = Cast<UWidgetBlueprintGeneratedClass>(Obj))
		{
			return Cast<UWidgetBlueprint>(WGC->ClassGeneratedBy);
		}
		return nullptr;
	}

	static UWidgetBlueprint* CreateWidgetBlueprintIfMissing(const FString& InPath)
	{
		FString Path = InPath;
		if (Path.EndsWith(TEXT("_C")))
		{
			Path = Path.LeftChop(2);
		}

		const int32 DotIndex = Path.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		const FString PackageName = (DotIndex >= 0) ? Path.Left(DotIndex) : Path;
		const FString AssetName = (DotIndex >= 0) ? Path.Mid(DotIndex + 1) : FPackageName::GetShortName(PackageName);
		const FString FolderPath = FPackageName::GetLongPackagePath(PackageName);

		if (PackageName.IsEmpty() || AssetName.IsEmpty() || FolderPath.IsEmpty())
		{
			return nullptr;
		}

		if (UWidgetBlueprint* Existing = LoadWidgetBlueprint(Path))
		{
			return Existing;
		}

		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->ParentClass = UUserWidget::StaticClass();

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		return Cast<UWidgetBlueprint>(AssetTools.CreateAsset(AssetName, FolderPath, UWidgetBlueprint::StaticClass(), Factory));
	}

	static bool ValidateAssertWidgets(UWidgetBlueprint* WidgetBP, const TSharedPtr<FJsonObject>& RootObj, FUMGAutoBuilderReport& Report)
	{
		if (!RootObj.IsValid() || !RootObj->HasTypedField<EJson::Array>(TEXT("assertWidgets")))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!RootObj->TryGetArrayField(TEXT("assertWidgets"), Arr) || !Arr)
		{
			return true;
		}

		UWidgetTree* Tree = WidgetBP ? WidgetBP->WidgetTree.Get() : nullptr;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const FString NameStr = V.IsValid() ? V->AsString() : FString();
			if (NameStr.IsEmpty())
			{
				continue;
			}

			if (!Tree || Tree->FindWidget(FName(*NameStr)) == nullptr)
			{
				AddError(Report, FString::Printf(TEXT("assertWidgets 失败：缺少控件 %s"), *NameStr));
			}
		}

		return !Report.HasErrors();
	}

	static TArray<TSharedPtr<FJsonValue>> Vec2ToJson(const FVector2D& V)
	{
		return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y) };
	}

	static TArray<TSharedPtr<FJsonValue>> MarginToJson(const FMargin& M)
	{
		return {
			MakeShared<FJsonValueNumber>(M.Left),
			MakeShared<FJsonValueNumber>(M.Top),
			MakeShared<FJsonValueNumber>(M.Right),
			MakeShared<FJsonValueNumber>(M.Bottom)
		};
	}

	static TArray<TSharedPtr<FJsonValue>> AnchorsToJson(const FAnchors& A)
	{
		return {
			MakeShared<FJsonValueNumber>(A.Minimum.X),
			MakeShared<FJsonValueNumber>(A.Minimum.Y),
			MakeShared<FJsonValueNumber>(A.Maximum.X),
			MakeShared<FJsonValueNumber>(A.Maximum.Y)
		};
	}

	static TArray<TSharedPtr<FJsonValue>> ColorToJson(const FLinearColor& C)
	{
		return {
			MakeShared<FJsonValueNumber>(C.R),
			MakeShared<FJsonValueNumber>(C.G),
			MakeShared<FJsonValueNumber>(C.B),
			MakeShared<FJsonValueNumber>(C.A)
		};
	}

	static void PutTexturePathIfAny(TSharedPtr<FJsonObject>& Props, UObject* ResourceObj)
	{
		if (!Props.IsValid() || !ResourceObj) return;
		Props->SetStringField(TEXT("texture"), ResourceObj->GetPathName());
	}

	static TSharedPtr<FJsonObject> ExportProps(const UWidget* Widget)
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		if (!Widget) return Props;

		Props->SetBoolField(TEXT("isVariable"), Widget->bIsVariable);
		Props->SetBoolField(TEXT("enabled"), Widget->GetIsEnabled());

		if (const UUserWidget* UW = Cast<UUserWidget>(Widget))
		{
			Props->SetStringField(TEXT("class"), UW->GetClass()->GetPathName());
		}
		else if (const UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			Props->SetStringField(TEXT("text"), Text->GetText().ToString());
			Props->SetNumberField(TEXT("fontSize"), Text->GetFont().Size);
			Props->SetArrayField(TEXT("color"), ColorToJson(Text->GetColorAndOpacity().GetSpecifiedColor()));
		}
		else if (const UImage* Image = Cast<UImage>(Widget))
		{
			const FSlateBrush Brush = Image->GetBrush();
			PutTexturePathIfAny(Props, Brush.GetResourceObject());
			Props->SetArrayField(TEXT("color"), ColorToJson(Image->GetColorAndOpacity()));
		}
		else if (const UBorder* Border = Cast<UBorder>(Widget))
		{
			PutTexturePathIfAny(Props, Border->Background.GetResourceObject());
			Props->SetArrayField(TEXT("brushColor"), ColorToJson(Border->GetBrushColor()));
			Props->SetArrayField(TEXT("padding"), MarginToJson(Border->GetPadding()));
		}
		else if (const UProgressBar* PB = Cast<UProgressBar>(Widget))
		{
			Props->SetNumberField(TEXT("percent"), PB->GetPercent());
			Props->SetArrayField(TEXT("fillColor"), ColorToJson(PB->GetFillColorAndOpacity()));
		}
		else if (const UButton* Button = Cast<UButton>(Widget))
		{
			Props->SetArrayField(TEXT("backgroundColor"), ColorToJson(Button->GetBackgroundColor()));
			Props->SetArrayField(TEXT("contentColor"), ColorToJson(Button->GetColorAndOpacity()));
		}
		else if (const USlider* Slider = Cast<USlider>(Widget))
		{
			Props->SetNumberField(TEXT("minValue"), Slider->GetMinValue());
			Props->SetNumberField(TEXT("maxValue"), Slider->GetMaxValue());
			Props->SetNumberField(TEXT("value"), Slider->GetValue());
			Props->SetNumberField(TEXT("stepSize"), Slider->GetStepSize());
			Props->SetArrayField(TEXT("barColor"), ColorToJson(Slider->GetSliderBarColor()));
			Props->SetArrayField(TEXT("handleColor"), ColorToJson(Slider->GetSliderHandleColor()));
		}
		else if (const USpinBox* SpinBox = Cast<USpinBox>(Widget))
		{
			Props->SetNumberField(TEXT("minValue"), SpinBox->GetMinValue());
			Props->SetNumberField(TEXT("maxValue"), SpinBox->GetMaxValue());
			Props->SetNumberField(TEXT("minSliderValue"), SpinBox->GetMinSliderValue());
			Props->SetNumberField(TEXT("maxSliderValue"), SpinBox->GetMaxSliderValue());
			Props->SetNumberField(TEXT("value"), SpinBox->GetValue());
			Props->SetNumberField(TEXT("delta"), SpinBox->GetDelta());
			Props->SetNumberField(TEXT("minFractionalDigits"), SpinBox->GetMinFractionalDigits());
			Props->SetNumberField(TEXT("maxFractionalDigits"), SpinBox->GetMaxFractionalDigits());
			Props->SetNumberField(TEXT("minDesiredWidth"), SpinBox->GetMinDesiredWidth());
			Props->SetArrayField(TEXT("foregroundColor"), ColorToJson(SpinBox->GetForegroundColor().GetSpecifiedColor()));
		}
		else if (const UCheckBox* CheckBox = Cast<UCheckBox>(Widget))
		{
			Props->SetBoolField(TEXT("checked"), CheckBox->IsChecked());
		}
		else if (const UComboBoxString* ComboBox = Cast<UComboBoxString>(Widget))
		{
			TArray<TSharedPtr<FJsonValue>> Options;
			for (int32 Index = 0; Index < ComboBox->GetOptionCount(); ++Index)
			{
				Options.Add(MakeShared<FJsonValueString>(ComboBox->GetOptionAtIndex(Index)));
			}
			Props->SetArrayField(TEXT("options"), Options);
			Props->SetStringField(TEXT("selectedOption"), ComboBox->GetSelectedOption());
		}
		else if (const UScrollBox* ScrollBox = Cast<UScrollBox>(Widget))
		{
			Props->SetBoolField(TEXT("alwaysShowScrollbar"), ScrollBox->IsAlwaysShowScrollbar());
			Props->SetBoolField(TEXT("allowOverscroll"), ScrollBox->IsAllowOverscroll());
			Props->SetBoolField(TEXT("animateWheelScrolling"), ScrollBox->IsAnimateWheelScrolling());
			Props->SetNumberField(TEXT("wheelScrollMultiplier"), ScrollBox->GetWheelScrollMultiplier());
		}
		else if (const UWrapBox* Wrap = Cast<UWrapBox>(Widget))
		{
			Props->SetArrayField(TEXT("innerSlotPadding"), Vec2ToJson(Wrap->GetInnerSlotPadding()));
			Props->SetNumberField(TEXT("wrapSize"), Wrap->GetWrapSize());
			Props->SetBoolField(TEXT("explicitWrapSize"), Wrap->UseExplicitWrapSize());
			Props->SetStringField(TEXT("horizontalAlignment"), ToHAlignString(Wrap->GetHorizontalAlignment()));
			Props->SetStringField(TEXT("orientation"), ToOrientationString(Wrap->GetOrientation()));
		}
		else if (const USizeBox* SB = Cast<USizeBox>(Widget))
		{
			const float W = SB->GetWidthOverride();
			if (!FMath::IsNearlyZero(W))
			{
				Props->SetNumberField(TEXT("widthOverride"), W);
			}

			const float H = SB->GetHeightOverride();
			if (!FMath::IsNearlyZero(H))
			{
				Props->SetNumberField(TEXT("heightOverride"), H);
			}
		}
		else if (const USpacer* Spacer = Cast<USpacer>(Widget))
		{
			Props->SetArrayField(TEXT("size"), Vec2ToJson(Spacer->GetSize()));
		}

		return Props;
	}

	static void ExportSlot(UPanelSlot* Slot, TSharedPtr<FJsonObject>& OutNode)
	{
		if (!Slot || !OutNode.IsValid()) return;

		TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();

		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			SlotObj->SetArrayField(TEXT("anchors"), AnchorsToJson(CanvasSlot->GetAnchors()));
			SlotObj->SetArrayField(TEXT("offsets"), MarginToJson(CanvasSlot->GetOffsets()));
			SlotObj->SetArrayField(TEXT("alignment"), Vec2ToJson(CanvasSlot->GetAlignment()));
			SlotObj->SetNumberField(TEXT("zOrder"), CanvasSlot->GetZOrder());
			SlotObj->SetBoolField(TEXT("autoSize"), CanvasSlot->GetAutoSize());
		}
		else if (UHorizontalBoxSlot* HSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			SlotObj->SetArrayField(TEXT("padding"), MarginToJson(HSlot->GetPadding()));
			SlotObj->SetStringField(TEXT("hAlign"), ToHAlignString(HSlot->GetHorizontalAlignment()));
			SlotObj->SetStringField(TEXT("vAlign"), ToVAlignString(HSlot->GetVerticalAlignment()));
			SlotObj->SetStringField(TEXT("sizeRule"), ToSizeRuleString(HSlot->GetSize()));
			SlotObj->SetNumberField(TEXT("fill"), HSlot->GetSize().Value);
		}
		else if (UVerticalBoxSlot* VSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			SlotObj->SetArrayField(TEXT("padding"), MarginToJson(VSlot->GetPadding()));
			SlotObj->SetStringField(TEXT("hAlign"), ToHAlignString(VSlot->GetHorizontalAlignment()));
			SlotObj->SetStringField(TEXT("vAlign"), ToVAlignString(VSlot->GetVerticalAlignment()));
			SlotObj->SetStringField(TEXT("sizeRule"), ToSizeRuleString(VSlot->GetSize()));
			SlotObj->SetNumberField(TEXT("fill"), VSlot->GetSize().Value);
		}
		else if (UOverlaySlot* OSlot = Cast<UOverlaySlot>(Slot))
		{
			SlotObj->SetArrayField(TEXT("padding"), MarginToJson(OSlot->GetPadding()));
			SlotObj->SetStringField(TEXT("hAlign"), ToHAlignString(OSlot->GetHorizontalAlignment()));
			SlotObj->SetStringField(TEXT("vAlign"), ToVAlignString(OSlot->GetVerticalAlignment()));
		}
		else if (UWrapBoxSlot* WSlot = Cast<UWrapBoxSlot>(Slot))
		{
			SlotObj->SetArrayField(TEXT("padding"), MarginToJson(WSlot->GetPadding()));
			SlotObj->SetStringField(TEXT("hAlign"), ToHAlignString(WSlot->GetHorizontalAlignment()));
			SlotObj->SetStringField(TEXT("vAlign"), ToVAlignString(WSlot->GetVerticalAlignment()));
			SlotObj->SetBoolField(TEXT("fillEmptySpace"), WSlot->DoesFillEmptySpace());
			SlotObj->SetBoolField(TEXT("newLine"), WSlot->DoesForceNewLine());
			SlotObj->SetNumberField(TEXT("fillSpanWhenLessThan"), WSlot->GetFillSpanWhenLessThan());
		}

		OutNode->SetObjectField(TEXT("slot"), SlotObj);
	}

	static TSharedPtr<FJsonObject> ExportWidgetNode(UWidget* Widget)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		if (!Widget) return Node;

		Node->SetStringField(TEXT("type"), Widget->IsA<UUserWidget>() ? TEXT("UserWidget") : Widget->GetClass()->GetName());

		// Normalize to the DSL names for supported widgets.
		if (Widget->IsA<UCanvasPanel>()) Node->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
		else if (Widget->IsA<UOverlay>()) Node->SetStringField(TEXT("type"), TEXT("Overlay"));
		else if (Widget->IsA<UVerticalBox>()) Node->SetStringField(TEXT("type"), TEXT("VerticalBox"));
		else if (Widget->IsA<UHorizontalBox>()) Node->SetStringField(TEXT("type"), TEXT("HorizontalBox"));
		else if (Widget->IsA<UWrapBox>()) Node->SetStringField(TEXT("type"), TEXT("WrapBox"));
		else if (Widget->IsA<USizeBox>()) Node->SetStringField(TEXT("type"), TEXT("SizeBox"));
		else if (Widget->IsA<UBorder>()) Node->SetStringField(TEXT("type"), TEXT("Border"));
		else if (Widget->IsA<UImage>()) Node->SetStringField(TEXT("type"), TEXT("Image"));
		else if (Widget->IsA<UTextBlock>()) Node->SetStringField(TEXT("type"), TEXT("TextBlock"));
		else if (Widget->IsA<UProgressBar>()) Node->SetStringField(TEXT("type"), TEXT("ProgressBar"));
		else if (Widget->IsA<UButton>()) Node->SetStringField(TEXT("type"), TEXT("Button"));
		else if (Widget->IsA<USlider>()) Node->SetStringField(TEXT("type"), TEXT("Slider"));
		else if (Widget->IsA<USpinBox>()) Node->SetStringField(TEXT("type"), TEXT("SpinBox"));
		else if (Widget->IsA<UCheckBox>()) Node->SetStringField(TEXT("type"), TEXT("CheckBox"));
		else if (Widget->IsA<UComboBoxString>()) Node->SetStringField(TEXT("type"), TEXT("ComboBoxString"));
		else if (Widget->IsA<UScrollBox>()) Node->SetStringField(TEXT("type"), TEXT("ScrollBox"));
		else if (Widget->IsA<USpacer>()) Node->SetStringField(TEXT("type"), TEXT("Spacer"));

		Node->SetStringField(TEXT("name"), Widget->GetFName().ToString());
		Node->SetObjectField(TEXT("props"), ExportProps(Widget));

		if (UPanelSlot* Slot = Widget->Slot)
		{
			ExportSlot(Slot, Node);
		}

		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			const int32 Count = Panel->GetChildrenCount();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (UWidget* Child = Panel->GetChildAt(Index))
				{
					Children.Add(MakeShared<FJsonValueObject>(ExportWidgetNode(Child)));
				}
			}
			if (Children.Num() > 0)
			{
				Node->SetArrayField(TEXT("children"), Children);
			}
		}

		return Node;
	}
}

bool FUMGAutoBuilderService::BuildFromConfigFile(const FString& ConfigPath, const FString& OverrideMode, FUMGAutoBuilderReport& OutReport)
{
	OutReport = FUMGAutoBuilderReport();

	FString ResolvedConfigPath = ConfigPath;
	ResolvedConfigPath.TrimQuotesInline();
	if (FPaths::IsRelative(ResolvedConfigPath))
	{
		ResolvedConfigPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), ResolvedConfigPath);
	}

	TSharedPtr<FJsonObject> RootObj;
	if (!GG_UMGAutoBuilder::LoadJsonObjectFromFile(ResolvedConfigPath, RootObj, OutReport))
	{
		return false;
	}

	GG_UMGAutoBuilder::ValidateSchemaAndTopLevel(RootObj, OutReport);
	if (OutReport.HasErrors())
	{
		return false;
	}

	FString TargetWidgetPath;
	if (!RootObj->TryGetStringField(TEXT("targetWidget"), TargetWidgetPath) || TargetWidgetPath.IsEmpty())
	{
		GG_UMGAutoBuilder::AddError(OutReport, TEXT("配置缺少 targetWidget"));
		return false;
	}

	FString Mode = OverrideMode;
	if (Mode.IsEmpty())
	{
		RootObj->TryGetStringField(TEXT("mode"), Mode);
	}
	if (Mode.IsEmpty())
	{
		Mode = TEXT("build");
	}

	const bool bPatch = Mode.Equals(TEXT("patch"), ESearchCase::IgnoreCase);
	const bool bValid = bPatch
		? GG_UMGAutoBuilder::ValidatePatchSpec(RootObj, OutReport)
		: GG_UMGAutoBuilder::ValidateBuildSpec(RootObj, OutReport);
	if (!bValid || OutReport.HasErrors())
	{
		return false;
	}

	UWidgetBlueprint* WidgetBP = GG_UMGAutoBuilder::LoadWidgetBlueprint(TargetWidgetPath);
	if (!WidgetBP)
	{
		bool bCreateIfMissing = false;
		RootObj->TryGetBoolField(TEXT("createIfMissing"), bCreateIfMissing);
		if (bCreateIfMissing)
		{
			WidgetBP = GG_UMGAutoBuilder::CreateWidgetBlueprintIfMissing(TargetWidgetPath);
		}

		if (!WidgetBP)
		{
			GG_UMGAutoBuilder::AddError(OutReport, FString::Printf(TEXT("无法加载/创建 WidgetBlueprint：%s"), *TargetWidgetPath));
			return false;
		}
	}

	const bool bApplied = bPatch
		? GG_UMGAutoBuilder::PatchWidgetBlueprintFromSpec(WidgetBP, RootObj, OutReport)
		: GG_UMGAutoBuilder::RebuildWidgetBlueprintFromSpec(WidgetBP, RootObj, OutReport);
	if (!bApplied || OutReport.HasErrors())
	{
		return false;
	}

	if (!GG_UMGAutoBuilder::ValidateAssertWidgets(WidgetBP, RootObj, OutReport))
	{
		return false;
	}

	if (!GG_UMGAutoBuilder::SaveAsset(WidgetBP, OutReport))
	{
		return false;
	}

	OutReport.Summary = FString::Printf(TEXT("%s: %s (config=%s)"),
		bPatch ? TEXT("Patch OK") : TEXT("Build OK"),
		*WidgetBP->GetPathName(),
		*ResolvedConfigPath);
	return true;
}

bool FUMGAutoBuilderService::ExportWidgetToJsonFile(const FString& WidgetPath, const FString& OutPath, bool bPretty, FUMGAutoBuilderReport& OutReport)
{
	OutReport = FUMGAutoBuilderReport();

	FString TrimmedWidgetPath = WidgetPath;
	TrimmedWidgetPath.TrimQuotesInline();

	UWidgetBlueprint* WidgetBP = GG_UMGAutoBuilder::LoadWidgetBlueprint(TrimmedWidgetPath);
	if (!WidgetBP || !WidgetBP->WidgetTree || !WidgetBP->WidgetTree->RootWidget)
	{
		GG_UMGAutoBuilder::AddError(OutReport, FString::Printf(TEXT("无法加载 WidgetBlueprint 或 WidgetTree 为空：%s"), *TrimmedWidgetPath));
		return false;
	}

	FString FinalOutPath = OutPath;
	FinalOutPath.TrimQuotesInline();
	if (FinalOutPath.IsEmpty())
	{
		const FString DefaultDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), TEXT("UMGAutoBuilderExport"));
		IFileManager::Get().MakeDirectory(*DefaultDir, true);
		FinalOutPath = DefaultDir / (WidgetBP->GetName() + TEXT(".json"));
	}
	else if (FPaths::IsRelative(FinalOutPath))
	{
		FinalOutPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), FinalOutPath);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), CurrentSchemaVersion);
	Root->SetStringField(TEXT("targetWidget"), WidgetBP->GetPathName());
	Root->SetStringField(TEXT("mode"), TEXT("build"));
	Root->SetObjectField(TEXT("root"), GG_UMGAutoBuilder::ExportWidgetNode(WidgetBP->WidgetTree->RootWidget));

	FString OutText;
	bool bSerializedOk = false;
	if (bPretty)
	{
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutText);
		bSerializedOk = FJsonSerializer::Serialize(Root.ToSharedRef(), Writer, true);
	}
	else
	{
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutText);
		bSerializedOk = FJsonSerializer::Serialize(Root.ToSharedRef(), Writer, true);
	}

	if (!bSerializedOk)
	{
		GG_UMGAutoBuilder::AddError(OutReport, TEXT("导出 JSON 序列化失败"));
		return false;
	}

	if (!FFileHelper::SaveStringToFile(OutText, *FinalOutPath, FFileHelper::EEncodingOptions::ForceUTF8))
	{
		GG_UMGAutoBuilder::AddError(OutReport, FString::Printf(TEXT("写入导出文件失败：%s"), *FinalOutPath));
		return false;
	}

	OutReport.Summary = FString::Printf(TEXT("Export OK: %s -> %s"), *WidgetBP->GetPathName(), *FinalOutPath);
	return true;
}

