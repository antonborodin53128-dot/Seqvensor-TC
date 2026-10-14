#include "MainComponent.h"

class TCApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "TC"; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (mainWindow != nullptr)
            mainWindow->toFront (true);
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (name, juce::Colour (0xff181b1f), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (760, 420, 4000, 3000);

            const auto stateFile = getStateFile();
            if (stateFile.existsAsFile())
                restoreWindowStateFromString (stateFile.loadFileAsString());
            else
                centreWithSize (getWidth(), getHeight());

            setVisible (true);
        }

        ~MainWindow() override
        {
            getStateFile().replaceWithText (getWindowStateAsString());
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        static juce::File getStateFile()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("TC");
            dir.createDirectory();
            return dir.getChildFile ("window.txt");
        }
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (TCApplication)
