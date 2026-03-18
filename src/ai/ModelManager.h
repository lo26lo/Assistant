#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace ibom::ai {

/**
 * @brief Manages AI model files (loading, caching, class labels).
 */
class ModelManager {
public:
    explicit ModelManager(const std::string& modelsDirectory = "models");
    ~ModelManager() = default;

    /// Scan models directory and register available models.
    void scanModels();

    /// Get the path to a model by name.
    std::string modelPath(const std::string& name) const;

    /// Load class names from a text file (one per line).
    bool loadClassNames(const std::string& path);

    /// Get class name by index.
    std::string className(int classId) const;

    /// Get all available model names.
    std::vector<std::string> availableModels() const;

    /// Get the models directory.
    const std::string& modelsDirectory() const { return m_modelsDir; }

private:
    std::string m_modelsDir;
    std::unordered_map<std::string, std::string> m_models; // name -> path
    std::vector<std::string> m_classNames;
};

} // namespace ibom::ai
