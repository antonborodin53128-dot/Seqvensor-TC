#pragma once

#include <juce_core/juce_core.h>

// Playlist = ordered list of sequence (song) names.
struct Playlist
{
    juce::String name;
    juce::StringArray items;
};

// Sequences and playlists live as small files in the app data folder.
class Library
{
public:
    explicit Library (const juce::File& dataDirectory)
        : sequenceDir (dataDirectory.getChildFile ("Sequences")), playlistDir (dataDirectory.getChildFile ("Playlists")),
          sessionFile (dataDirectory.getChildFile ("session.xml"))
    {
        sequenceDir.createDirectory();
        playlistDir.createDirectory();
    }

    static juce::String sanitize (juce::String s)
    {
        s = s.removeCharacters ("\\/:*?\"<>|").trim();
        return s.isEmpty() ? juce::String ("Untitled") : s;
    }

    static juce::String uniqueName (const juce::StringArray& existing, const juce::String& base)
    {
        if (! existing.contains (base))
            return base;
        for (int i = 2; i < 10000; ++i)
        {
            auto candidate = base + " " + juce::String (i);
            if (! existing.contains (candidate))
                return candidate;
        }
        return base;
    }

    juce::File sequenceFile (const juce::String& name) const { return sequenceDir.getChildFile (sanitize (name) + ".xml"); }
    juce::File playlistFile (const juce::String& name) const { return playlistDir.getChildFile (sanitize (name) + ".xml"); }

    juce::StringArray sequenceNames() const { return names (sequenceDir); }
    juce::StringArray playlistNames() const { return names (playlistDir); }

    bool loadPlaylist (const juce::String& name, Playlist& out) const
    {
        auto xml = juce::XmlDocument::parse (playlistFile (name));
        if (xml == nullptr || ! xml->hasTagName ("TC_PLAYLIST"))
            return false;
        out.name = name;
        out.items.clear();
        for (auto* e : xml->getChildWithTagNameIterator ("ITEM"))
            out.items.add (e->getStringAttribute ("seq"));
        return true;
    }

    void savePlaylist (const Playlist& p) const
    {
        juce::XmlElement root ("TC_PLAYLIST");
        for (auto& s : p.items)
            root.createNewChildElement ("ITEM")->setAttribute ("seq", s);
        playlistFile (p.name).replaceWithText (root.toString());
    }

    void deletePlaylist (const juce::String& name) const { playlistFile (name).deleteFile(); }

    // a sequence was renamed / deleted: fix every playlist that mentions it
    void renameInPlaylists (const juce::String& from, const juce::String& to) const
    {
        for (auto& pn : playlistNames())
        {
            Playlist p;
            if (! loadPlaylist (pn, p))
                continue;
            bool changed = false;
            for (auto& s : p.items)
                if (s == from)
                {
                    s = to;
                    changed = true;
                }
            if (changed)
                savePlaylist (p);
        }
    }

    void removeFromPlaylists (const juce::String& name) const
    {
        for (auto& pn : playlistNames())
        {
            Playlist p;
            if (loadPlaylist (pn, p) && p.items.contains (name))
            {
                p.items.removeString (name);
                savePlaylist (p);
            }
        }
    }

    void saveSession (const juce::String& sequence, const juce::String& playlist) const
    {
        juce::XmlElement root ("TC_SESSION");
        root.setAttribute ("sequence", sequence);
        root.setAttribute ("playlist", playlist);
        sessionFile.replaceWithText (root.toString());
    }

    void loadSession (juce::String& sequence, juce::String& playlist) const
    {
        if (auto xml = juce::XmlDocument::parse (sessionFile))
        {
            sequence = xml->getStringAttribute ("sequence");
            playlist = xml->getStringAttribute ("playlist");
        }
    }

private:
    static juce::StringArray names (const juce::File& dir)
    {
        juce::StringArray result;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
            result.add (f.getFileNameWithoutExtension());
        result.sort (true);
        return result;
    }

    juce::File sequenceDir, playlistDir, sessionFile;
};
