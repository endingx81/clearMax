#include "SpaceAnalyzer.h"
#include <windows.h>
#include <algorithm>
#include <PortableDeviceApi.h>
#include <PortableDevice.h>

SpaceAnalyzer::SpaceAnalyzer() : m_scannedFiles(0), m_lastTimeMs(0) {}

SpaceAnalyzer::~SpaceAnalyzer() {}

void SpaceAnalyzer::scanPath(const std::wstring& path, std::function<void(int, const std::wstring&)> progressCallback, std::function<bool()> cancelCheck) {
    m_scannedFiles = 0;
    m_lastTimeMs = GetTickCount64();
    
    // Normalize path (ensure no trailing slash unless it's a root like C:\)
    std::wstring rootName = path;
    if (rootName.length() > 3 && (rootName.back() == L'\\' || rootName.back() == L'/')) {
        rootName.pop_back();
    }
    
    m_root = std::make_unique<DirNode>(rootName, true, nullptr);
    
    scanRecursive(m_root.get(), rootName, progressCallback, cancelCheck);
}

void SpaceAnalyzer::scanRecursive(DirNode* node, const std::wstring& path, std::function<void(int, const std::wstring&)>& progressCallback, std::function<bool()>& cancelCheck) {
    if (cancelCheck && cancelCheck()) return;

    std::wstring searchPath = path;
    if (searchPath.back() != L'\\') searchPath += L"\\";
    searchPath += L"*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::vector<std::pair<DirNode*, std::wstring>> dirsToScan;

    do {
        if (cancelCheck && cancelCheck()) break;
        
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        
        auto child = std::make_unique<DirNode>(fd.cFileName, isDir, node);
        DirNode* childPtr = child.get();
        
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            node->children.push_back(std::move(child));
        }
        
        if (isDir) {
            std::wstring subPath = path;
            if (subPath.back() != L'\\') subPath += L"\\";
            subPath += fd.cFileName;
            dirsToScan.push_back({childPtr, subPath});
        } else {
            uint64_t fileSize = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            childPtr->size = fileSize;
            childPtr->count = 1;
            
            node->addSize(fileSize, 1);
            m_scannedFiles++;
        }
        
        uint64_t now = GetTickCount64();
        if (now - m_lastTimeMs > 100) {
            m_lastTimeMs = now;
            if (progressCallback) {
                progressCallback(0, path);
            }
        }
        
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    for (auto& dir : dirsToScan) {
        if (cancelCheck && cancelCheck()) break;
        scanRecursive(dir.first, dir.second, progressCallback, cancelCheck);
    }
}

std::vector<DirNode*> SpaceAnalyzer::getSortedChildren(DirNode* node, SortMode mode) {
    std::vector<DirNode*> result;
    if (!node) return result;
    
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& child : node->children) {
            result.push_back(child.get());
        }
    }
    
    std::sort(result.begin(), result.end(), [mode](DirNode* a, DirNode* b) {
        if (a->isDir != b->isDir) return a->isDir; // Folders first
        if (mode == SortMode::SizeDesc) {
            return a->size > b->size;
        } else {
            return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0;
        }
    });
    return result;
}

void SpaceAnalyzer::removeNode(DirNode* node) {
    if (!node || !node->parent) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    node->parent->removeSize(node->size, node->count);
    auto& siblings = node->parent->children;
    siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
        [node](const std::unique_ptr<DirNode>& child) { return child.get() == node; }), siblings.end());
}

bool SpaceAnalyzer::scanMtpDevice(const std::wstring& deviceId, const std::wstring& deviceName, std::function<void(int, const std::wstring&)> progressCallback, std::function<bool()> cancelCheck) {
    m_scannedFiles = 0;
    m_lastTimeMs = GetTickCount64();

    m_root = std::make_unique<DirNode>(deviceName, true, nullptr);

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool needCoUninit = SUCCEEDED(hr);

    IPortableDevice* pDevice = nullptr;
    hr = CoCreateInstance(CLSID_PortableDeviceFTM, NULL, CLSCTX_INPROC_SERVER, IID_IPortableDevice, (void**)&pDevice);
    if (FAILED(hr) || !pDevice) {
        if (needCoUninit) CoUninitialize();
        return false;
    }

    IPortableDeviceValues* pClientInfo = nullptr;
    CoCreateInstance(CLSID_PortableDeviceValues, NULL, CLSCTX_INPROC_SERVER, IID_IPortableDeviceValues, (void**)&pClientInfo);
    if (pClientInfo) {
        pClientInfo->SetStringValue(WPD_CLIENT_NAME, L"clearMax");
        pClientInfo->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
        pClientInfo->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 16);
    }

    hr = pDevice->Open(deviceId.c_str(), pClientInfo);
    if (pClientInfo) pClientInfo->Release();

    if (FAILED(hr)) {
        pDevice->Release();
        if (needCoUninit) CoUninitialize();
        return false;
    }

    IPortableDeviceContent* pContent = nullptr;
    hr = pDevice->Content(&pContent);
    if (SUCCEEDED(hr) && pContent) {
        IPortableDeviceProperties* pProps = nullptr;
        hr = pContent->Properties(&pProps);
        if (SUCCEEDED(hr) && pProps) {
            scanMtpRecursive(pContent, pProps, WPD_DEVICE_OBJECT_ID, m_root.get(), progressCallback, cancelCheck);
            pProps->Release();
        }
        pContent->Release();
    }

    pDevice->Close();
    pDevice->Release();

    if (needCoUninit) CoUninitialize();
    return true;
}

void SpaceAnalyzer::scanMtpRecursive(void* pContentPtr, void* pPropsPtr, const wchar_t* parentId, DirNode* parentNode, std::function<void(int, const std::wstring&)>& progressCallback, std::function<bool()>& cancelCheck) {
    if (cancelCheck && cancelCheck()) return;

    IPortableDeviceContent* pContent = static_cast<IPortableDeviceContent*>(pContentPtr);
    IPortableDeviceProperties* pProps = static_cast<IPortableDeviceProperties*>(pPropsPtr);

    IEnumPortableDeviceObjectIDs* pEnum = nullptr;
    HRESULT hr = pContent->EnumObjects(0, parentId, NULL, &pEnum);
    if (FAILED(hr) || !pEnum) return;

    struct DirToScan {
        DirNode* node;
        std::wstring objectId;
        std::wstring name;
    };
    std::vector<DirToScan> subDirs;

    const DWORD BATCH_SIZE = 32;
    LPWSTR objectIDs[BATCH_SIZE];
    DWORD cFetched = 0;

    while (SUCCEEDED(pEnum->Next(BATCH_SIZE, objectIDs, &cFetched)) && cFetched > 0) {
        for (DWORD i = 0; i < cFetched; ++i) {
            if (cancelCheck && cancelCheck()) {
                CoTaskMemFree(objectIDs[i]);
                continue;
            }

            IPortableDeviceValues* pValues = nullptr;
            hr = pProps->GetValues(objectIDs[i], NULL, &pValues);
            if (SUCCEEDED(hr) && pValues) {
                LPWSTR name = nullptr;
                pValues->GetStringValue(WPD_OBJECT_NAME, &name);
                std::wstring itemName = (name && name[0]) ? name : L"항목";

                ULONGLONG size = 0;
                pValues->GetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, &size);

                GUID contentType = {};
                pValues->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &contentType);

                bool isFolder = IsEqualGUID(contentType, WPD_CONTENT_TYPE_FOLDER) ||
                                IsEqualGUID(contentType, WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT);

                auto child = std::make_unique<DirNode>(itemName, isFolder, parentNode);
                DirNode* childPtr = child.get();

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    parentNode->children.push_back(std::move(child));
                }

                if (isFolder) {
                    subDirs.push_back({ childPtr, objectIDs[i], itemName });
                } else {
                    childPtr->size = size;
                    childPtr->count = 1;
                    parentNode->addSize(size, 1);
                    m_scannedFiles++;
                }

                uint64_t now = GetTickCount64();
                if (now - m_lastTimeMs > 80) {
                    m_lastTimeMs = now;
                    if (progressCallback) {
                        progressCallback(static_cast<int>(m_scannedFiles), itemName);
                    }
                }

                if (name) CoTaskMemFree(name);
                pValues->Release();
            }
            CoTaskMemFree(objectIDs[i]);
        }
    }
    pEnum->Release();

    for (const auto& dir : subDirs) {
        if (cancelCheck && cancelCheck()) break;
        scanMtpRecursive(pContentPtr, pPropsPtr, dir.objectId.c_str(), dir.node, progressCallback, cancelCheck);
    }
}
