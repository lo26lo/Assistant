#include "ModelManager.h"

#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>

namespace ibom::ai {

namespace fs = std::filesystem;

ModelManager::ModelManager(const std::string& modelsDirectory)
    : m_modelsDir(modelsDirectory)
{
    scanModels();
}

void ModelManager::scanModels()
{
    m_models.clear();

    if (!fs::exists(m_modelsDir)) {
        spdlog::warn("Models directory '{}' does not exist.", m_modelsDir);
        return;
    }

    for (const auto& entry : fs::directory_iterator(m_modelsDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".onnx") {
            std::string name = entry.path().stem().string();
            m_models[name] = entry.path().string();
            spdlog::debug("Found model: {} -> {}", name, entry.path().string());
        }
    }

    // Try to load class names if a .txt file with same name exists
    for (const auto& [name, path] : m_models) {
        fs::path labelsPath = fs::path(path).replace_extension(".txt");
        if (fs::exists(labelsPath)) {
            loadClassNames(labelsPath.string());
            break; // Use first found
        }
    }

    spdlog::info("Found {} models in '{}'", m_models.size(), m_modelsDir);
}

std::string ModelManager::modelPath(const std::string& name) const
{
    auto it = m_models.find(name);
    if (it != m_models.end()) return it->second;
    return {};
}

bool ModelManager::loadClassNames(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        spdlog::error("Cannot open class names file: {}", path);
        return false;
    }

    m_classNames.clear();
    std::string line;
    while (std::getline(ifs, line)) {
        // Trim
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (!line.empty()) {
            m_classNames.push_back(line);
        }
    }

    spdlog::info("Loaded {} class names from '{}'", m_classNames.size(), path);
    return true;
}

std::string ModelManager::className(int classId) const
{
    if (classId >= 0 && static_cast<size_t>(classId) < m_classNames.size()) {
        return m_classNames[classId];
    }
    return "class_" + std::to_string(classId);
}

std::vector<std::string> ModelManager::availableModels() const
{
    std::vector<std::string> names;
    names.reserve(m_models.size());
    for (const auto& [name, path] : m_models) {
        names.push_back(name);
    }
    return names;
}

} // namespace ibom::ai
