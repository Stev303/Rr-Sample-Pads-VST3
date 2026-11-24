#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>
#include <functional>

class RrSamplePadsProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int padCount = 16;
    struct Pad
    {
        std::shared_ptr<juce::AudioBuffer<float>> audio;
        std::atomic<double> position { -1.0 };
        std::atomic<float> gain { 1.0f };
        std::atomic<float> velocity { 1.0f };
        std::atomic<int> note { 36 };
        juce::String name;
        std::atomic<double> sourceRate { 44100.0 };
    };

    RrSamplePadsProcessor()
        : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        formats.registerBasicFormats();
        for (int i = 0; i < padCount; ++i)
        {
            pads[(size_t)i].name = "PAD " + juce::String(i + 1);
            pads[(size_t)i].note.store(36 + i);
        }
    }

    const juce::String getName() const override { return "Rr Sample Pads"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void prepareToPlay(double sr, int) override { currentRate = sr; }
    void releaseResources() override { stopAll(); }
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        const auto out = layouts.getMainOutputChannelSet();
        const auto in = layouts.getMainInputChannelSet();
        return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
            && (in == juce::AudioChannelSet::disabled() || in == out);
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (const auto metadata : midi)
        {
            const auto msg = metadata.getMessage();
            if (msg.isNoteOn())
                for (int i = 0; i < padCount; ++i)
                    if (msg.getNoteNumber() == pads[(size_t)i].note.load())
                        triggerPad(i, msg.getFloatVelocity());
            // MIDI note-off intentionally does not stop one-shot samples.
        }

        for (int i = 0; i < padCount; ++i)
        {
            auto& p = pads[(size_t)i];
            auto sample = std::atomic_load(&p.audio);
            if (!sample) continue;
            double pos = p.position.load();
            if (pos < 0.0) continue;

            const double sr = p.sourceRate.load();
            const double step = (sr > 0.0 && currentRate > 0.0) ? sr / currentRate : 1.0;
            const float gain = p.gain.load() * p.velocity.load() * master.load();
            const int sourceChannels = sample->getNumChannels();
            const int sourceLength = sample->getNumSamples();
            if (sourceChannels <= 0 || sourceLength <= 0) { p.position.store(-1.0); continue; }

            for (int n = 0; n < buffer.getNumSamples(); ++n)
            {
                const int i0 = (int)pos;
                if (i0 >= sourceLength) { pos = -1.0; break; }
                const int i1 = juce::jmin(i0 + 1, sourceLength - 1);
                const float frac = (float)(pos - i0);
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                {
                    const int srcCh = juce::jmin(ch, sourceChannels - 1);
                    const float a = sample->getSample(srcCh, i0);
                    const float b = sample->getSample(srcCh, i1);
                    buffer.addSample(ch, n, (a + (b - a) * frac) * gain);
                }
                pos += step;
            }
            p.position.store(pos >= sourceLength ? -1.0 : pos);
        }
    }

    void triggerPad(int index, float velocity = 1.0f)
    {
        if (index < 0 || index >= padCount) return;
        auto sample = std::atomic_load(&pads[(size_t)index].audio);
        if (sample)
        {
            pads[(size_t)index].velocity.store(juce::jlimit(0.0f, 1.0f, velocity));
            pads[(size_t)index].position.store(0.0);
        }
    }

    void stopAll() { for (auto& p : pads) p.position.store(-1.0); }

    bool loadSample(int index, const juce::File& file)
    {
        if (index < 0 || index >= padCount || !file.existsAsFile()) return false;
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (!reader || reader->lengthInSamples <= 0 || reader->lengthInSamples > 100000000
            || reader->numChannels == 0) return false;

        auto data = std::make_shared<juce::AudioBuffer<float>>((int)reader->numChannels, (int)reader->lengthInSamples);
        if (!reader->read(data.get(), 0, (int)reader->lengthInSamples, 0, true, true)) return false;

        std::atomic_store(&pads[(size_t)index].audio, data);
        pads[(size_t)index].sourceRate.store(reader->sampleRate);
        pads[(size_t)index].name = file.getFileNameWithoutExtension();
        pads[(size_t)index].position.store(-1.0);
        return true;
    }

    void getStateInformation(juce::MemoryBlock& dest) override
    {
        juce::ValueTree root("RrSamplePads");
        root.setProperty("master", master.load(), nullptr);
        for (int i = 0; i < padCount; ++i)
        {
            auto& p = pads[(size_t)i];
            juce::ValueTree node("Pad");
            node.setProperty("index", i, nullptr);
            node.setProperty("name", p.name, nullptr);
            node.setProperty("gain", p.gain.load(), nullptr);
            auto audio = std::atomic_load(&p.audio);
            if (audio)
            {
                juce::MemoryBlock bytes;
                juce::MemoryOutputStream stream(bytes, false);
                juce::WavAudioFormat wav;
                const double rate = p.sourceRate.load() > 0.0 ? p.sourceRate.load() : 44100.0;
                std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(
                    &stream, rate, (unsigned int)audio->getNumChannels(), 24, {}, 0));
                if (writer)
                {
                    writer->writeFromAudioSampleBuffer(*audio, 0, audio->getNumSamples());
                    writer.reset();
                    node.setProperty("audio", bytes.toBase64Encoding(), nullptr);
                }
            }
            root.addChild(node, -1, nullptr);
        }
        if (auto xml = root.createXml()) copyXmlToBinary(*xml, dest);
    }

    void setStateInformation(const void* data, int size) override
    {
        auto xml = getXmlFromBinary(data, size);
        if (!xml) return;
        auto root = juce::ValueTree::fromXml(*xml);
        if (!root.isValid()) return;
        master.store((float)root.getProperty("master", 1.0));
        for (int i = 0; i < root.getNumChildren(); ++i)
        {
            auto node = root.getChild(i);
            const int index = (int)node.getProperty("index", -1);
            if (index < 0 || index >= padCount) continue;
            auto& p = pads[(size_t)index];
            p.name = node.getProperty("name", "PAD " + juce::String(index + 1)).toString();
            p.gain.store(juce::jlimit(0.0f, 2.0f, (float)node.getProperty("gain", 1.0)));
            juce::MemoryBlock bytes;
            if (bytes.fromBase64Encoding(node.getProperty("audio").toString()))
            {
                juce::MemoryInputStream stream(bytes, false);
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(&stream, false));
                if (reader && reader->lengthInSamples > 0 && reader->numChannels > 0)
                {
                    auto audio = std::make_shared<juce::AudioBuffer<float>>((int)reader->numChannels, (int)reader->lengthInSamples);
                    if (reader->read(audio.get(), 0, (int)reader->lengthInSamples, 0, true, true))
                    {
                        std::atomic_store(&p.audio, audio);
                        p.sourceRate.store(reader->sampleRate);
                    }
                }
            }
        }
    }

    std::array<Pad, padCount> pads;
    std::atomic<float> master { 1.0f };
    double currentRate = 44100.0;
    juce::AudioFormatManager formats;
};

class SamplePadView final : public juce::Component
{
public:
    SamplePadView(RrSamplePadsProcessor& p, int padIndex, std::function<void()> select)
        : proc(p), index(padIndex), onSelect(std::move(select))
    {
        addAndMakeVisible(play);
        play.setButtonText("▶");
        play.onClick = [this] { onSelect(); proc.triggerPad(index); };
        play.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff242936));
        play.setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff2a34a));

        addAndMakeVisible(load);
        load.setButtonText("LOAD");
        load.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff343b4a));
        load.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffd8dbe2));
        load.onClick = [this] { onSelect(); if (onLoad) onLoad(index); };

        addAndMakeVisible(name);
        name.setJustificationType(juce::Justification::centred);
        name.setColour(juce::Label::textColourId, juce::Colour(0xffd9dce4));
        name.setFont(juce::Font(12.0f, juce::Font::bold));

        addAndMakeVisible(volume);
        volume.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        volume.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 54, 16);
        volume.setTextValueSuffix(" %");
        volume.setRange(0.0, 200.0, 1.0);
        volume.setValue(proc.pads[(size_t)index].gain.load() * 100.0, juce::dontSendNotification);
        volume.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffe99a3c));
        volume.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff555b69));
        volume.setColour(juce::Slider::textBoxTextColourId, juce::Colour(0xffcbd0da));
        volume.onValueChange = [this] { proc.pads[(size_t)index].gain.store((float)(volume.getValue() / 100.0)); };
    }

    void setSelected(bool s) { selected = s; repaint(); }
    void refreshName()
    {
        auto audio = std::atomic_load(&proc.pads[(size_t)index].audio);
        name.setText(audio ? proc.pads[(size_t)index].name : "Drop sample", juce::dontSendNotification);
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(juce::Colour(0xff20232c));
        g.fillRoundedRectangle(r, 9.0f);
        g.setColour(selected ? juce::Colour(0xfff19a38) : juce::Colour(0xff414653));
        g.drawRoundedRectangle(r, 9.0f, selected ? 2.2f : 1.0f);
        g.setColour(juce::Colour(0xffaeb5c2));
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawText("PAD " + juce::String(index + 1), 8, 5, getWidth() - 16, 17, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        area.removeFromTop(23);
        auto bottom = area.removeFromBottom(28);
        load.setBounds(bottom.removeFromRight(58));
        name.setBounds(area.removeFromTop(22));
        play.setBounds(area.removeFromTop(34).withSizeKeepingCentre(42, 32));
        volume.setBounds(area.reduced(3));
    }

    std::function<void(int)> onLoad;

private:
    RrSamplePadsProcessor& proc;
    int index;
    std::function<void()> onSelect;
    bool selected = false;
    juce::TextButton play, load;
    juce::Label name;
    juce::Slider volume;
};

class RrSamplePadsEditor final : public juce::AudioProcessorEditor, private juce::FileDragAndDropTarget
{
public:
    explicit RrSamplePadsEditor(RrSamplePadsProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        setSize(900, 700);
        setResizable(true, true);
        for (int i = 0; i < 16; ++i)
        {
            auto* pad = pads.add(new SamplePadView(proc, i, [this, i] { selectedPad = i; updateSelection(); }));
            addAndMakeVisible(pad);
            pad->onLoad = [this](int index) { chooseFile(index); };
            pad->refreshName();
        }

        addAndMakeVisible(title);
        title.setText("RR SAMPLE PADS", juce::dontSendNotification);
        title.setFont(juce::Font(20.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colour(0xfff0a044));

        addAndMakeVisible(subtitle);
        subtitle.setText("16 SLOT SAMPLE PLAYER", juce::dontSendNotification);
        subtitle.setFont(juce::Font(10.0f, juce::Font::bold));
        subtitle.setColour(juce::Label::textColourId, juce::Colour(0xff858d9c));

        addAndMakeVisible(masterLabel);
        masterLabel.setText("MASTER", juce::dontSendNotification);
        masterLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd5d9e2));
        masterSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        masterSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 55, 20);
        masterSlider.setRange(0.0, 200.0, 1.0);
        masterSlider.setValue(proc.master.load() * 100.0, juce::dontSendNotification);
        masterSlider.setTextValueSuffix(" %");
        masterSlider.onValueChange = [this] { proc.master.store((float)(masterSlider.getValue() / 100.0)); };
        addAndMakeVisible(masterSlider);

        addAndMakeVisible(stopButton);
        stopButton.setButtonText("STOP ALL");
        stopButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff49313a));
        stopButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffffb5b5));
        stopButton.onClick = [this] { proc.stopAll(); };

        addAndMakeVisible(status);
        status.setText("Double-click LOAD or drag an audio file onto a pad", juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, juce::Colour(0xff8992a1));
        status.setJustificationType(juce::Justification::centredLeft);
        updateSelection();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff171a21));
        g.setColour(juce::Colour(0xff252a34));
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(8.0f), 12.0f);
        g.setColour(juce::Colour(0xff343b49));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(8.0f), 12.0f, 1.0f);
        g.setColour(juce::Colour(0xff343b49));
        g.drawHorizontalLine(62, 18.0f, (float)getWidth() - 18.0f);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(18);
        auto header = bounds.removeFromTop(48);
        title.setBounds(header.removeFromTop(25));
        subtitle.setBounds(header);
        bounds.removeFromTop(12);

        auto footer = bounds.removeFromBottom(58);
        status.setBounds(footer.removeFromTop(20));
        auto controls = footer.reduced(2);
        masterLabel.setBounds(controls.removeFromLeft(75));
        masterSlider.setBounds(controls.removeFromLeft(250));
        stopButton.setBounds(controls.removeFromRight(105).reduced(2));

        const int gap = 10;
        const int cellW = (bounds.getWidth() - gap * 3) / 4;
        const int cellH = (bounds.getHeight() - gap * 3) / 4;
        for (int i = 0; i < 16; ++i)
            pads[i]->setBounds(bounds.getX() + (i % 4) * (cellW + gap),
                               bounds.getY() + (i / 4) * (cellH + gap), cellW, cellH);
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return files.size() > 0;
    }

    void filesDropped(const juce::StringArray& files, int x, int y) override
    {
        for (int i = 0; i < pads.size(); ++i)
        {
            if (pads[i]->getBounds().contains(x, y) && files.size() > 0)
            {
                selectedPad = i;
                proc.loadSample(i, juce::File(files[0]));
                pads[i]->refreshName();
                updateSelection();
                return;
            }
        }
    }

private:
    void chooseFile(int index)
    {
        chooser = std::make_unique<juce::FileChooser>("Load sample into Pad " + juce::String(index + 1),
            juce::File{}, "*.wav;*.mp3;*.aiff;*.aif;*.flac;*.ogg");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, index](const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file.existsAsFile() && proc.loadSample(index, file))
                {
                    selectedPad = index;
                    pads[index]->refreshName();
                    status.setText("Loaded: " + file.getFileName(), juce::dontSendNotification);
                    updateSelection();
                }
            });
    }

    void updateSelection()
    {
        for (int i = 0; i < pads.size(); ++i) pads[i]->setSelected(i == selectedPad);
    }

    RrSamplePadsProcessor& proc;
    juce::OwnedArray<SamplePadView> pads;
    juce::Label title, subtitle, masterLabel, status;
    juce::Slider masterSlider;
    juce::TextButton stopButton;
    std::unique_ptr<juce::FileChooser> chooser;
    int selectedPad = 0;
};

juce::AudioProcessorEditor* RrSamplePadsProcessor::createEditor() { return new RrSamplePadsEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RrSamplePadsProcessor(); }
