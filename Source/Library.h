#pragma once

#include "Text.h"

#include <juce_core/juce_core.h>

// Playlist ("Сет 1") = ordered list of composition names inside one sequence.
struct Playlist
{
    juce::String name;
    juce::StringArray items;
};

// Layout on disk:   Projects / <sequence, e.g. a band> / Compositions / <composition>.xml
//                                                       / Playlists    / <playlist>.xml
class Library
{
public:
    explicit Library (const juce::File& dataDirectory)
        : root (dataDirectory.getChildFile ("Projects")), legacyCompositions (dataDirectory.getChildFile ("Sequences")),
          legacyPlaylists (dataDirectory.getChildFile ("Playlists")), sessionFile (dataDirectory.getChildFile ("session.xml"))
    {
        root.createDirectory();
        migrate();
    }

    // ---- names
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

    // ---- sequences (projects)
    juce::StringArray projectNames() const
    {
        juce::StringArray result;
        for (auto& d : root.findChildFiles (juce::File::findDirectories, false))
            result.add (d.getFileName());
        result.sort (true);
        return result;
    }

    const juce::String& getProject() const { return project; }

    void setProject (const juce::String& name)
    {
        project = name;
        compositionDir().createDirectory();
        playlistDir().createDirectory();
    }

    juce::File projectDir (const juce::String& name) const { return root.getChildFile (sanitize (name)); }

    bool createProject (const juce::String& name) const { return projectDir (name).getChildFile ("Compositions").createDirectory().wasOk(); }
    bool renameProject (const juce::String& from, const juce::String& to)
    {
        if (! projectDir (from).moveFileTo (projectDir (to)))
            return false;
        if (project == from)
            project = to;
        return true;
    }
    void deleteProject (const juce::String& name) const { projectDir (name).deleteRecursively(); }

    // ---- compositions (songs) of the current sequence
    juce::File sequenceFile (const juce::String& name) const { return compositionDir().getChildFile (sanitize (name) + ".xml"); }
    juce::StringArray sequenceNames() const { return names (compositionDir()); }

    // ---- playlists of the current sequence
    juce::File playlistFile (const juce::String& name) const { return playlistDir().getChildFile (sanitize (name) + ".xml"); }
    juce::StringArray playlistNames() const { return names (playlistDir()); }

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
        juce::XmlElement rootXml ("TC_PLAYLIST");
        for (auto& s : p.items)
            rootXml.createNewChildElement ("ITEM")->setAttribute ("seq", s);
        playlistFile (p.name).replaceWithText (rootXml.toString());
    }

    void deletePlaylist (const juce::String& name) const { playlistFile (name).deleteFile(); }

    // a composition was renamed / deleted: fix every playlist that mentions it
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

    // ---- last session
    void saveSession (const juce::String& composition, const juce::String& playlist) const
    {
        juce::XmlElement r ("TC_SESSION");
        r.setAttribute ("project", project);
        r.setAttribute ("composition", composition);
        r.setAttribute ("playlist", playlist);
        sessionFile.replaceWithText (r.toString());
    }

    void loadSession (juce::String& projectOut, juce::String& composition, juce::String& playlist) const
    {
        if (auto xml = juce::XmlDocument::parse (sessionFile))
        {
            projectOut = xml->getStringAttribute ("project");
            composition = xml->getStringAttribute ("composition", xml->getStringAttribute ("sequence"));
            playlist = xml->getStringAttribute ("playlist");
        }
    }

private:
    juce::File compositionDir() const { return root.getChildFile (sanitize (project)).getChildFile ("Compositions"); }
    juce::File playlistDir() const { return root.getChildFile (sanitize (project)).getChildFile ("Playlists"); }

    // older versions kept songs / playlists in two flat folders: they become the first sequence
    void migrate()
    {
        if (! projectNames().isEmpty())
            return;

        const bool hasSongs = ! legacyCompositions.findChildFiles (juce::File::findFiles, false, "*.xml").isEmpty();
        const bool hasLists = ! legacyPlaylists.findChildFiles (juce::File::findFiles, false, "*.xml").isEmpty();
        if (! hasSongs && ! hasLists)
            return;

        const auto target = root.getChildFile ("Моя секвенция"_ru);
        auto comps = target.getChildFile ("Compositions");
        auto lists = target.getChildFile ("Playlists");
        comps.createDirectory();
        lists.createDirectory();

        for (auto& f : legacyCompositions.findChildFiles (juce::File::findFiles, false, "*.xml"))
            f.moveFileTo (comps.getChildFile (f.getFileName()));
        for (auto& f : legacyPlaylists.findChildFiles (juce::File::findFiles, false, "*.xml"))
            f.moveFileTo (lists.getChildFile (f.getFileName()));
    }

    static juce::StringArray names (const juce::File& dir)
    {
        juce::StringArray result;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
            result.add (f.getFileNameWithoutExtension());
        result.sort (true);
        return result;
    }

    juce::File root, legacyCompositions, legacyPlaylists, sessionFile;
    juce::String project;
};
