//* UI & Graphics related stuff *//

#pragma once
#include <JuceHeader.h>

// ==========================================
// UI constants
// ==========================================
namespace SetupUI
{
    const juce::Colour background = juce::Colour (0xff222326);
    const juce::Colour labelsColor = juce::Colour (0xffB0B0B0);

    // Slider tracks
    const juce::Colour sliderTrackGreen = juce::Colour (0xff488c0d);
    const juce::Colour sliderTrackOrange = juce::Colour (0xffbd631e);
    const juce::Colour sliderTrackPurple = juce::Colour (0xff4b0b5c);
    const juce::Colour sliderTrackBlue = juce::Colour (0xff126fa6);
    const juce::Colour sliderTrackDarkGreen = juce::Colour (0xff2c5707);

    constexpr int toggleSize = 22;

    enum class LedColour
    {
        Red,
        Green,
        Orange,
        Blue,
        Purple
    };
}

inline std::unique_ptr<juce::Drawable> loadSvgFromBinary (const void* data, size_t size)
{
    auto xml = juce::XmlDocument::parse (
        juce::String::fromUTF8 ((const char*) data, (int) size));

    return juce::Drawable::createFromSVG (*xml);
}

inline const void* getOnSvgData (SetupUI::LedColour c)
{
    switch (c)
    {
        case SetupUI::LedColour::Red:    return BinaryData::checkbox_on_red_svg;
        case SetupUI::LedColour::Green:  return BinaryData::checkbox_on_green_svg;
        case SetupUI::LedColour::Orange: return BinaryData::checkbox_on_orange_svg;
        case SetupUI::LedColour::Purple: return BinaryData::checkbox_on_purple_svg;
        case SetupUI::LedColour::Blue:   return BinaryData::checkbox_on_blue_svg;
    }

    return nullptr;
}

inline int getOnSvgSize (SetupUI::LedColour c)
{
    switch (c)
    {
        case SetupUI::LedColour::Red:    return BinaryData::checkbox_on_red_svgSize;
        case SetupUI::LedColour::Green:  return BinaryData::checkbox_on_green_svgSize;
        case SetupUI::LedColour::Orange: return BinaryData::checkbox_on_orange_svgSize;
        case SetupUI::LedColour::Purple: return BinaryData::checkbox_on_purple_svgSize;
        case SetupUI::LedColour::Blue:   return BinaryData::checkbox_on_blue_svgSize;
    }

    return 0;
}

inline std::unique_ptr<juce::Drawable> loadOffSvg()
{
    return loadSvgFromBinary (BinaryData::checkbox_off_svg,
                              BinaryData::checkbox_off_svgSize);
}

// ==========================================
// Image based toggle button
// ==========================================
class LedToggleButton : public juce::DrawableButton
{
    public:
        LedToggleButton (const juce::String& name,
                         SetupUI::LedColour colour)
            : juce::DrawableButton (name, juce::DrawableButton::ImageStretched),
              offDrawable (loadOffSvg()),
              onDrawable  (loadSvgFromBinary (getOnSvgData (colour),
                                              getOnSvgSize (colour)))
        {
            jassert (offDrawable && onDrawable);

            setClickingTogglesState (true);

            setImages (offDrawable.get(), nullptr, nullptr, nullptr,
                       onDrawable.get(),  nullptr, nullptr, nullptr);
        }

    private:
        std::unique_ptr<juce::Drawable> offDrawable;
        std::unique_ptr<juce::Drawable> onDrawable;
};

// ==========================================
// Custom LookAndFeel (sliders)
// ==========================================
class ModzTaktLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // optional: pick a default LED accent for sliders
    explicit ModzTaktLookAndFeel (juce::Colour accent = juce::Colour (0xff3CFF6B))
        : accentColour (accent) {}

    void setAccentColour (juce::Colour c) { accentColour = c; }

    // NEW: Set an outline colour for a specific slider
    void setSliderOutline (juce::Slider* slider, juce::Colour outlineColour, float thickness = 1.5f)
    {
        if (slider != nullptr)
        {
            sliderOutlines[slider] = { outlineColour, thickness };
        }
    }

    // NEW: Clear the outline for a specific slider
    void clearSliderOutline (juce::Slider* slider)
    {
        if (slider != nullptr)
        {
            sliderOutlines.erase (slider);
        }
    }

    void drawLinearSlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos,
                           float minSliderPos,
                           float maxSliderPos,
                           const juce::Slider::SliderStyle style,
                           juce::Slider& slider) override
    {
        juce::ignoreUnused (minSliderPos, maxSliderPos);

        // Only handle linear sliders here
        if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height,
                                                   sliderPos, minSliderPos, maxSliderPos,
                                                   style, slider);
            return;
        }

        const auto trackWidth   = 6.0f;
        const auto handleSize   = 10.0f;

        const auto trackBg = juce::Colour (0xff141414);
        const auto handle  = juce::Colour (0xff636363);
        const auto border  = juce::Colour (0xffb0aeb0);

        // Check if this slider has an outline defined
        auto outlineIt = sliderOutlines.find (&slider);
        bool hasOutline = (outlineIt != sliderOutlines.end());
        juce::Colour outlineColour;
        float outlineThickness = 1.3f;
        
        if (hasOutline)
        {
            outlineColour = outlineIt->second.colour;
            outlineThickness = outlineIt->second.thickness;
        }

        if (style == juce::Slider::LinearVertical)
        {
            const float centreX = (float) x + (float) width * 0.5f;

            juce::Rectangle<float> track (centreX - trackWidth * 0.5f,
                                          (float) y,
                                          trackWidth,
                                          (float) height);

            g.setColour (trackBg);
            g.fillRoundedRectangle (track, 3.0f);

            // Filled portion (from sliderPos down to bottom)
            juce::Rectangle<float> fill (track.getX(),
                                         sliderPos,
                                         trackWidth,
                                         track.getBottom() - sliderPos);

            g.setColour (accentColour);
            g.fillRoundedRectangle (fill, 3.0f);

            // NEW: Draw outline around the filled track if defined
            if (hasOutline)
            {
                g.setColour (outlineColour);
                g.drawRoundedRectangle (fill, 3.0f, outlineThickness);
            }

            // Handle block
            juce::Rectangle<float> knob (centreX - 8.0f,
                                         sliderPos - handleSize * 0.5f,
                                         16.0f,
                                         handleSize);

            g.setColour (handle);
            g.fillRoundedRectangle (knob, 3.0f);

            g.setColour (border);
            g.drawRoundedRectangle (knob, 3.0f, 1.0f);
        }
        else // LinearHorizontal
        {
            const float centreY = (float) y + (float) height * 0.5f;

            juce::Rectangle<float> track ((float) x,
                                          centreY - trackWidth * 0.5f,
                                          (float) width,
                                          trackWidth);

            g.setColour (trackBg);
            g.fillRoundedRectangle (track, 3.0f);

            // Filled portion (from left to sliderPos)
            juce::Rectangle<float> fill (track.getX(),
                                         track.getY(),
                                         sliderPos - track.getX(),
                                         trackWidth);

            g.setColour (accentColour);
            g.fillRoundedRectangle (fill, 3.0f);

            // NEW: Draw outline around the filled track if defined
            if (hasOutline)
            {
                g.setColour (outlineColour);
                g.drawRoundedRectangle (fill, 3.0f, outlineThickness);
            }

            // Handle block
            juce::Rectangle<float> knob (sliderPos - handleSize * 0.5f,
                                         centreY - 8.0f,
                                         handleSize,
                                         16.0f);

            g.setColour (handle);
            g.fillRoundedRectangle (knob, 3.0f);

            g.setColour (border);
            g.drawRoundedRectangle (knob, 3.0f, 1.0f);
        }
    }

private:
    juce::Colour accentColour;
    
    // NEW: Store outline information per slider
    struct OutlineInfo
    {
        juce::Colour colour;
        float thickness;
    };
    std::map<juce::Slider*, OutlineInfo> sliderOutlines;
};