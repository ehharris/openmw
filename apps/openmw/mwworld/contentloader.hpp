#ifndef CONTENTLOADER_HPP
#define CONTENTLOADER_HPP

#include <filesystem>

namespace Loading
{
    class Listener;
}

namespace MWWorld
{

    // Content loading pipeline for the game's data files. Files are dispatched by
    // extension (see World::loadContentFiles in worldimp.cpp). Oblivion-specific
    // (TES4) content is validated and summarized by Oblivion::ContentLoader
    // (components/oblivion/contentloader.hpp) from within EsmLoader before the
    // records are handed over to the generic TES4-family reader, so invalid or
    // misordered Oblivion plugins are diagnosed with Oblivion-specific messages.

    struct ContentLoader
    {
        virtual ~ContentLoader() = default;

        virtual void load(const std::filesystem::path& filepath, int& index, Loading::Listener* listener) = 0;
    };

} /* namespace MWWorld */

#endif /* CONTENTLOADER_HPP */
