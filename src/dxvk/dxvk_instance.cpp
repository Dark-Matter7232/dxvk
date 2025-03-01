#include <version.h>

#include "dxvk_instance.h"
#include "dxvk_openvr.h"
#include "dxvk_openxr.h"
#include "dxvk_platform_exts.h"

#include <algorithm>
#include <sstream>

namespace dxvk {
  
  DxvkInstance::DxvkInstance() {
    Logger::info(str::format("Game: ", env::getExeName()));
    Logger::info(str::format("DXVK: ", DXVK_VERSION));

    m_config = Config::getUserConfig();
    m_config.merge(Config::getAppConfig(env::getExePath()));
    m_config.logOptions();

    m_options = DxvkOptions(m_config);

    m_extProviders.push_back(&DxvkPlatformExts::s_instance);
#ifdef _WIN32
    m_extProviders.push_back(&VrInstance::s_instance);
    m_extProviders.push_back(&DxvkXrProvider::s_instance);
#endif

    Logger::info("Built-in extension providers:");
    for (const auto& provider : m_extProviders)
      Logger::info(str::format("  ", provider->getName()));

    for (const auto& provider : m_extProviders)
      provider->initInstanceExtensions();

    m_vkl = new vk::LibraryFn();
    if (!m_vkl->valid())
      throw DxvkError("Failed to load vulkan-1 library.");
    m_vki = new vk::InstanceFn(m_vkl, true, this->createInstance());

    if (m_enableValidation) {
      VkDebugUtilsMessengerCreateInfoEXT messengerInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
      messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      messengerInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
      messengerInfo.pfnUserCallback = &debugCallback;

      if (m_vki->vkCreateDebugUtilsMessengerEXT(m_vki->instance(), &messengerInfo, nullptr, &m_messenger))
        Logger::err("DxvkInstance::createInstance: Failed to create debug messenger, proceeding without.");
    }

    m_adapters = this->queryAdapters();

    for (const auto& provider : m_extProviders)
      provider->initDeviceExtensions(this);

    for (uint32_t i = 0; i < m_adapters.size(); i++) {
      for (const auto& provider : m_extProviders) {
        m_adapters[i]->enableExtensions(
          provider->getDeviceExtensions(i));
      }
    }
  }
  
  
  DxvkInstance::~DxvkInstance() {
    if (m_messenger)
      m_vki->vkDestroyDebugUtilsMessengerEXT(m_vki->instance(), m_messenger, nullptr);
  }
  
  
  Rc<DxvkAdapter> DxvkInstance::enumAdapters(uint32_t index) const {
    return index < m_adapters.size()
      ? m_adapters[index]
      : nullptr;
  }


  Rc<DxvkAdapter> DxvkInstance::findAdapterByLuid(const void* luid) const {
    for (const auto& adapter : m_adapters) {
      const auto& vk11 = adapter->devicePropertiesExt().vk11;

      if (vk11.deviceLUIDValid && !std::memcmp(luid, vk11.deviceLUID, VK_LUID_SIZE))
        return adapter;
    }

    return nullptr;
  }

  
  Rc<DxvkAdapter> DxvkInstance::findAdapterByDeviceId(uint16_t vendorId, uint16_t deviceId) const {
    for (const auto& adapter : m_adapters) {
      const auto& props = adapter->deviceProperties();

      if (props.vendorID == vendorId
       && props.deviceID == deviceId)
        return adapter;
    }

    return nullptr;
  }
  
  
  VkInstance DxvkInstance::createInstance() {
    DxvkInstanceExtensions insExtensions;

    std::vector<DxvkExt*> insExtensionList = {{
      &insExtensions.khrGetSurfaceCapabilities2,
      &insExtensions.khrSurface,
    }};

    // Hide VK_EXT_debug_utils behind an environment variable. This extension
    // adds additional overhead to winevulkan
    std::string debugEnv = env::getEnvVar("DXVK_DEBUG");
    DxvkNameList layerNameList;

    m_enablePerfEvents = debugEnv == "markers";
    m_enableValidation = debugEnv == "validation";

    if (m_enablePerfEvents || m_enableValidation || m_options.enableDebugUtils) {
      insExtensionList.push_back(&insExtensions.extDebugUtils);
      Logger::warn("Debug Utils are enabled. May affect performance.");

      if (m_enableValidation) {
        const char* layerName = "VK_LAYER_KHRONOS_validation";
        DxvkNameSet layers = DxvkNameSet::enumInstanceLayers(m_vkl);

        if (layers.supports(layerName)) {
          layerNameList.add(layerName);
          Logger::warn(str::format("Enabled instance layer ", layerName));
        } else {
          // This can happen on winevulkan since it does not support layers
          Logger::warn(str::format("Validation layers not found, set VK_INSTANCE_LAYERS=", layerName));
        }
      }
    }

    DxvkNameSet extensionsEnabled;
    DxvkNameSet extensionsAvailable = DxvkNameSet::enumInstanceExtensions(m_vkl);
    
    if (!extensionsAvailable.enableExtensions(
          insExtensionList.size(),
          insExtensionList.data(),
          extensionsEnabled))
      throw DxvkError("DxvkInstance: Failed to create instance");

    m_extensions = insExtensions;

    // Enable additional extensions if necessary
    for (const auto& provider : m_extProviders)
      extensionsEnabled.merge(provider->getInstanceExtensions());

    DxvkNameList extensionNameList = extensionsEnabled.toNameList();
    
    Logger::info("Enabled instance extensions:");
    this->logNameList(extensionNameList);

    std::string appName = env::getExeName();
    
    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName      = appName.c_str();
    appInfo.pEngineName           = "DXVK";
    appInfo.engineVersion         = VK_MAKE_VERSION(2, 1, 0);
    appInfo.apiVersion            = VK_MAKE_VERSION(1, 3, 0);
    
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo         = &appInfo;
    info.enabledLayerCount        = layerNameList.count();
    info.ppEnabledLayerNames      = layerNameList.names();
    info.enabledExtensionCount    = extensionNameList.count();
    info.ppEnabledExtensionNames  = extensionNameList.names();
    
    VkInstance result = VK_NULL_HANDLE;
    VkResult status = m_vkl->vkCreateInstance(&info, nullptr, &result);

    if (status != VK_SUCCESS)
      throw DxvkError("DxvkInstance::createInstance: Failed to create Vulkan 1.1 instance");

    return result;
  }
  
  
  std::vector<Rc<DxvkAdapter>> DxvkInstance::queryAdapters() {
    uint32_t numAdapters = 0;
    if (m_vki->vkEnumeratePhysicalDevices(m_vki->instance(), &numAdapters, nullptr) != VK_SUCCESS)
      throw DxvkError("DxvkInstance::enumAdapters: Failed to enumerate adapters");
    
    std::vector<VkPhysicalDevice> adapters(numAdapters);
    if (m_vki->vkEnumeratePhysicalDevices(m_vki->instance(), &numAdapters, adapters.data()) != VK_SUCCESS)
      throw DxvkError("DxvkInstance::enumAdapters: Failed to enumerate adapters");

    std::vector<VkPhysicalDeviceProperties> deviceProperties(numAdapters);
    DxvkDeviceFilterFlags filterFlags = 0;

    for (uint32_t i = 0; i < numAdapters; i++) {
      m_vki->vkGetPhysicalDeviceProperties(adapters[i], &deviceProperties[i]);

      if (deviceProperties[i].deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
        filterFlags.set(DxvkDeviceFilterFlag::SkipCpuDevices);
    }

    DxvkDeviceFilter filter(filterFlags);
    std::vector<Rc<DxvkAdapter>> result;

    for (uint32_t i = 0; i < numAdapters; i++) {
      if (filter.testAdapter(deviceProperties[i]))
        result.push_back(new DxvkAdapter(m_vki, adapters[i]));
    }
    
    std::stable_sort(result.begin(), result.end(),
      [] (const Rc<DxvkAdapter>& a, const Rc<DxvkAdapter>& b) -> bool {
        static const std::array<VkPhysicalDeviceType, 3> deviceTypes = {{
          VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
          VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
          VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
        }};

        uint32_t aRank = deviceTypes.size();
        uint32_t bRank = deviceTypes.size();

        for (uint32_t i = 0; i < std::min(aRank, bRank); i++) {
          if (a->deviceProperties().deviceType == deviceTypes[i]) aRank = i;
          if (b->deviceProperties().deviceType == deviceTypes[i]) bRank = i;
        }

        return aRank < bRank;
      });
    
    if (result.size() == 0) {
      Logger::warn("DXVK: No adapters found. Please check your "
                   "device filter settings and Vulkan setup.");
    }
    
    return result;
  }
  
  
  void DxvkInstance::logNameList(const DxvkNameList& names) {
    for (uint32_t i = 0; i < names.count(); i++)
      Logger::info(str::format("  ", names.name(i)));
  }
  

  VkBool32 VKAPI_CALL DxvkInstance::debugCallback(
          VkDebugUtilsMessageSeverityFlagBitsEXT  messageSeverity,
          VkDebugUtilsMessageTypeFlagsEXT         messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT*   pCallbackData,
          void*                                   pUserData) {
    LogLevel logLevel;

    switch (messageSeverity) {
      default:
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:    logLevel = LogLevel::Info;  break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT: logLevel = LogLevel::Debug; break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT: logLevel = LogLevel::Warn;  break;
      case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:   logLevel = LogLevel::Error; break;
    }

    static const std::array<uint32_t, 8> ignoredIds = {
      // Ignore image format features for depth-compare instructions.
      // These errors are expected in D3D9 and some D3D11 apps.
      0x23259a0d,
      0x4b9d1597,
      0x534c50ad,
      0x9750b479,
      // Ignore vkCmdBindPipeline errors related to dynamic rendering.
      // Validation layers are buggy here and will complain about any
      // command buffer with more than one render pass.
      0x11b37e31,
      0x151f5e5a,
      0x6c16bfb4,
      0xd6d77e1e,
    };

    for (auto id : ignoredIds) {
      if (uint32_t(pCallbackData->messageIdNumber) == id)
        return VK_FALSE;
    }

    std::stringstream str;

    if (pCallbackData->pMessageIdName)
      str << pCallbackData->pMessageIdName << ": " << std::endl;

    str << pCallbackData->pMessage;

    Logger::log(logLevel, str.str());
    return VK_FALSE;
  }

}
