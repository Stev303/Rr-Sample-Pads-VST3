#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>

class RrSamplePadsProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int padCount = 16;
    struct Pad
    {
        std::shared_ptr<juce::AudioBuffer<float>> audio;
        std::atomic<int> position { -1 };
        std::atomic<float> gain { 1.0f };
        std::atomic<int> note { 36 };
        std::atomic<int> choke { 0 };
        std::atomic<bool> normalise { false };
        juce::String name;
        double sourceRate = 44100.0;
    };

    RrSamplePadsProcessor()
        : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)
                                           .withInput("Input", juce::AudioChannelSet::stereo(), true))
    {
        formats.registerBasicFormats();
        for (int i = 0; i < padCount; ++i) { pads[(size_t)i].name = "Pad " + juce::String(i + 1); pads[(size_t)i].note = 36 + i; }
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
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
            || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        buffer.clear();
        for (const auto metadata : midi)
        {
            const auto msg = metadata.getMessage();
            if (msg.isNoteOn())
                for (int i = 0; i < padCount; ++i)
                    if (msg.getNoteNumber() == pads[(size_t)i].note.load())
                        triggerPad(i, msg.getFloatVelocity());
            else if (msg.isAllNotesOff() || msg.isAllSoundOff()) stopAll();
        }

        for (int i = 0; i < padCount; ++i)
        {
            auto& p = pads[(size_t)i];
            auto sample = std::atomic_load(&p.audio);
            if (!sample) continue;
            int pos = p.position.load();
            if (pos < 0) continue;
            const float gain = p.gain.load() * master.load();
            const int n = juce::jmin(buffer.getNumSamples(), sample->getNumSamples() - pos);
            if (n <= 0) { p.position.store(-1); continue; }
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const int srcCh = juce::jmin(ch, sample->getNumChannels() - 1);
                buffer.addFrom(ch, 0, *sample, srcCh, pos, n, gain);
            }
            const int next = pos + n;
            p.position.store(next >= sample->getNumSamples() ? -1 : next);
        }
    }

    void triggerPad(int index, float velocity = 1.0f)
    {
        if (index < 0 || index >= padCount) return;
        const int group = pads[(size_t)index].choke.load();
        if (group > 0)
            for (int i = 0; i < padCount; ++i)
                if (i != index && pads[(size_t)i].choke.load() == group) pads[(size_t)i].position.store(-1);
        auto sample = std::atomic_load(&pads[(size_t)index].audio);
        if (sample) { pads[(size_t)index].gain.store(velocity); pads[(size_t)index].position.store(0); }
    }
    void stopAll() { for (auto& p : pads) p.position.store(-1); }
    bool loadSample(int index, const juce::File& file)
    {
        if (index < 0 || index >= padCount || !file.existsAsFile()) return false;
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (!reader || reader->lengthInSamples <= 0 || reader->lengthInSamples > 100000000) return false;
        auto data = std::make_shared<juce::AudioBuffer<float>>((int)reader->numChannels, (int)reader->lengthInSamples);
        reader->read(data.get(), 0, (int)reader->lengthInSamples, 0, true, true);
        if (pads[(size_t)index].normalise.load())
        {
            const float peak = data->getMagnitude(0, data->getNumSamples());
            if (peak > 0.00001f) data->applyGain(0.999f / peak);
        }
        std::atomic_store(&pads[(size_t)index].audio, data);
        pads[(size_t)index].sourceRate = reader->sampleRate;
        pads[(size_t)index].name = file.getFileNameWithoutExtension();
        pads[(size_t)index].position.store(-1);
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
            node.setProperty("note", p.note.load(), nullptr);
            node.setProperty("gain", p.gain.load(), nullptr);
            node.setProperty("choke", p.choke.load(), nullptr);
            node.setProperty("normalise", p.normalise.load(), nullptr);
            auto data = std::atomic_load(&p.audio);
            if (data)
            {
                juce::MemoryBlock audioBytes;
                juce::WavAudioFormat wav;
                juce::MemoryOutputStream stream(audioBytes, false);
                std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(&stream, currentRate > 0 ? currentRate : 44100.0, (unsigned int)data->getNumChannels(), 24, {}, 0));
                if (writer) { writer->writeFromAudioSampleBuffer(*data, 0, data->getNumSamples()); writer.reset(); node.setProperty("audio", juce::var(audioBytes.toBase64Encoding()), nullptr); }
            }
            root.addChild(node, -1, nullptr);
        }
        std::unique_ptr<juce::XmlElement> xml(root.createXml());
        copyXmlToBinary(*xml, dest);
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
            p.name = node.getProperty("name", "Pad " + juce::String(index + 1)).toString();
            p.note.store((int)node.getProperty("note", 36 + index));
            p.gain.store((float)node.getProperty("gain", 1.0));
            p.choke.store((int)node.getProperty("choke", 0));
            p.normalise.store((bool)node.getProperty("normalise", false));
            juce::MemoryBlock bytes;
            if (bytes.fromBase64Encoding(node.getProperty("audio").toString()))
            {
                juce::MemoryInputStream stream(bytes, false);
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(&stream, false));
                if (reader && reader->lengthInSamples > 0)
                {
                    auto audio = std::make_shared<juce::AudioBuffer<float>>((int)reader->numChannels, (int)reader->lengthInSamples);
                    reader->read(audio.get(), 0, (int)reader->lengthInSamples, 0, true, true);
                    std::atomic_store(&p.audio, audio);
                }
            }
        }
    }

    std::array<Pad, padCount> pads;
    std::atomic<float> master { 1.0f };
    double currentRate = 44100.0;
    juce::AudioFormatManager formats;
};

class PadButton final : public juce::TextButton
{
public:
    std::function<void()> onLoad;
    void mouseDoubleClick(const juce::MouseEvent&) override { if (onLoad) onLoad(); }
};

class RrSamplePadsEditor final : public juce::AudioProcessorEditor, private juce::FileDragAndDropTarget
{
public:
    explicit RrSamplePadsEditor(RrSamplePadsProcessor& p) : AudioProcessorEditor(p), proc(p)
    {
        setSize(740, 620);
        setResizable(true, true);
        for (int i = 0; i < 16; ++i)
        {
            auto* b = pads.add(new PadButton());
            b->setButtonText(juce::String(i + 1) + "\nDrop sample");
            b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff242329));
            b->setColour(juce::TextButton::textColourOffId, juce::Colour(0xffaaa8b2));
            b->onClick = [this, i] { proc.triggerPad(i); };
            b->onLoad = [this, i] { chooseFile(i); };
            b->setClickingTogglesState(false);
        }
        addAndMakeVisible(masterLabel);
        masterLabel.setText("Master", juce::dontSendNotification);
        addAndMakeVisible(masterSlider);
        masterSlider.setRange(0.0, 2.0, 0.01);
        masterSlider.setValue(1.0);
        masterSlider.onValueChange = [this] { proc.master.store((float)masterSlider.getValue()); };
        addAndMakeVisible(stopButton);
        stopButton.setButtonText("Stop All");
        stopButton.onClick = [this] { proc.stopAll(); };
        addAndMakeVisible(hint);
        hint.setText("Double-click a pad to load audio  |  Drag & drop audio files onto pads", juce::dontSendNotification);
        hint.setJustificationType(juce::Justification::centred);
    }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff343238));
        g.setColour(juce::Colour(0xffd2a15e));
        g.setFont(juce::Font(18.0f, juce::Font::bold));
        g.drawText("Sample Pads", 16, 7, getWidth() - 32, 26, juce::Justification::centredLeft);
    }
    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        area.removeFromTop(32);
        auto footer = area.removeFromBottom(48);
        hint.setBounds(footer.removeFromTop(20));
        masterLabel.setBounds(footer.removeFromLeft(55));
        masterSlider.setBounds(footer.removeFromLeft(180).reduced(2));
        stopButton.setBounds(footer.removeFromRight(100).reduced(2));
        const int gap = 7;
        const int w = (area.getWidth() - gap * 3) / 4;
        const int h = (area.getHeight() - gap * 3) / 4;
        for (int i = 0; i < 16; ++i)
            pads[i]->setBounds(area.getX() + (i % 4) * (w + gap), area.getY() + (i / 4) * (h + gap), w, h);
    }
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return files.size() > 0;
    }
    void filesDropped(const juce::StringArray& files, int x, int y) override
    {
        const int col = juce::jlimit(0, 3, (x - 10) / juce::jmax(1, (getWidth() - 26) / 4));
        const int row = juce::jlimit(0, 3, (y - 42) / juce::jmax(1, (getHeight() - 110) / 4));
        const int index = row * 4 + col;
        if (files.size() > 0) proc.loadSample(index, juce::File(files[0]));
        refreshLabels();
    }
private:
    void chooseFile(int index)
    {
        chooser = std::make_unique<juce::FileChooser>("Load sample", juce::File{}, "*.wav;*.mp3;*.aiff;*.aif;*.flac;*.ogg");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, index](const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.existsAsFile()) proc.loadSample(index, f);
                refreshLabels();
            });
    }
    void refreshLabels()
    {
        for (int i = 0; i < 16; ++i)
        {
            auto data = std::atomic_load(&proc.pads[(size_t)i].audio);
            pads[i]->setButtonText(juce::String(i + 1) + "\n" + (data ? proc.pads[(size_t)i].name : "Drop sample"));
        }
    }
    RrSamplePadsProcessor& proc;
    juce::OwnedArray<PadButton> pads;
    juce::Label masterLabel, hint;
    juce::Slider masterSlider;
    juce::TextButton stopButton;
    std::unique_ptr<juce::FileChooser> chooser;
};

juce::AudioProcessorEditor* RrSamplePadsProcessor::createEditor() { return new RrSamplePadsEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RrSamplePadsProcessor(); }
