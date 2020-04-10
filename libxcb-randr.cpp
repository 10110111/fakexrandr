/*
    FakeXRandR
    Copyright (c) 2020, Ruslan Kabatsayev
    Copyright (c) 2015, Phillip Berndt

    This is a replacement library for libxcb-randr.so. It replaces configurable
    outputs with multiple sub-outputs.
*/

#include <new>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sys/mman.h>
#include <xcb/xcbext.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <algorithm>
#include <type_traits>

#include "xcb-randr-orig-lib.h"
#include "fakexrandr.h"

extern "C"
{
/*
    The skeleton file is created by ./make_skeleton.py

    It contains wrappers around all XCB RandR functions which are not
    explicitly defined in this C file, replacing all references to
    crtcs and outputs which are fake with the real ones.
*/
#include "skeleton-xcb-randr.h"
}

namespace
{

// These new & delete replacement functions are here to avoid linking to libstdc++
template<typename T, typename...Args>
T* newObj(Args&&...args)
{
    T* p=static_cast<T*>(malloc(sizeof(T)));
    new (p) T(std::forward<Args>(args)...);
    return p;
}

template<typename T>
T* newArr(size_t size)
{
    return static_cast<T*>(malloc(size*sizeof(T)));
}

template<typename T>
void deleteObj(T* p)
{
    p->~T();
    free(p);
}

template<typename T>
int list_length(T* list)
{
    int i = 0;
    while(list)
    {
        list = list->nextInList;
        i += 1;
    }
    return i;
}

template<typename T>
void free_list(T* list)
{
    while(list)
    {
        T* last = list;
        list = list->nextInList;
        deleteObj(last);
    }
}

template<typename T>
bool xid_in_list(T* list, uint32_t xid)
{
    while(list)
    {
        if(list->xid == xid || list->parent_xid == xid)
            return true;
        list = list->nextInList;
    }
    return false;
}

template<typename T>
struct List
{
    struct ListItem
    {
        T data;
        ListItem* nextInList=nullptr;
        ListItem(T&& data, ListItem* nextInList)
            : data(data)
            , nextInList(nextInList)
        {
        }
    };
    ListItem* first=nullptr;
    void prepend(T&& v)
    {
        const auto oldFirst=first;
        first=newObj<ListItem>(std::move(v),oldFirst);
    }
    void erase(ListItem*const item)
    {
        for(ListItem *curr=first, *prev=NULL; curr; prev=curr, curr=curr->nextInList)
        {
            if(curr!=item) continue;
            (prev ? prev->nextInList : first) = curr->nextInList;
            deleteObj(curr);
            return;
        }
    }
    template<typename Match>
    ListItem* find(Match const& matches)
    {
        for(ListItem* curr=first; curr; curr=curr->nextInList)
            if(matches(curr->data))
                return curr;
        return nullptr;
    }
};

template<typename Key, typename Value>
class AssocList
{
public:
    struct KeyValuePair
    {
        Key key;
        Value value;
    };
    using ListItem=typename List<KeyValuePair>::ListItem;
    void insert(Key const& key, Value const& value)
    {
        pairs.prepend(KeyValuePair{key, value});
    }
    ListItem* find(Key const& key)
    {
        return pairs.find([&](KeyValuePair const& p){return p.key==key;});
    }
    void erase(ListItem* pair)
    {
        pairs.erase(pair);
    }
private:
    List<KeyValuePair> pairs;
};

struct FakeCrtcInfo
{
    FakeCrtcInfo* nextInList=nullptr;
    uint32_t xid;

    xcb_randr_get_crtc_info_reply_t orig_crtc_info;
    xcb_randr_output_t output;

    FakeCrtcInfo(const uint32_t xid, xcb_randr_output_t const& output, xcb_randr_get_crtc_info_reply_t const& crtcInfo,
                 const int16_t x, const int16_t y, const uint16_t width, const uint16_t height, xcb_randr_mode_t const& mode)
        : xid(xid)
        , orig_crtc_info(crtcInfo)
        , output(output)
    {
        orig_crtc_info.num_outputs = orig_crtc_info.num_possible_outputs = 1;
        orig_crtc_info.x = x;
        orig_crtc_info.y = y;
        orig_crtc_info.width = width;
        orig_crtc_info.height = height;
        orig_crtc_info.mode = mode;
    }
    xcb_randr_get_crtc_info_reply_t* makeReturnValue() const
    {
        const auto begin=&orig_crtc_info;
        const auto end=_xcb_randr_get_crtc_info_possible(begin)+orig_crtc_info.num_possible_outputs;
        const auto size=reinterpret_cast<const char*>(end)-reinterpret_cast<const char*>(begin);
        const auto p=static_cast<xcb_randr_get_crtc_info_reply_t*>(malloc(size));
        // Copy fixed-size part
        *p=orig_crtc_info;
        // Now fill in the variable-length data
        *_xcb_randr_get_crtc_info_outputs(p)=output;
        *_xcb_randr_get_crtc_info_possible(p)=output;
        return p;
    }
};

struct FakeOutputInfo
{
    FakeOutputInfo* nextInList=nullptr;
    uint32_t xid;
    uint32_t parent_xid;

    xcb_randr_get_output_info_reply_t orig_output_info;
    xcb_randr_mode_t* modes=nullptr;
    xcb_randr_output_t* clones=nullptr;
    uint8_t* name=nullptr;

    FakeOutputInfo(const uint32_t xid, const uint32_t parent_xid, xcb_randr_get_output_info_reply_t const& origInfo,
                   const uint16_t num_modes, const uint16_t num_clones,
                   const uint32_t mm_width, const uint32_t mm_height, const unsigned suffixIndex)
        : xid(xid)
        , parent_xid(parent_xid)
        , orig_output_info(origInfo)
        , modes(newArr<xcb_randr_mode_t>(num_modes))
        , clones(newArr<xcb_randr_output_t>(num_clones))
    {
        orig_output_info.mm_width=mm_width;
        orig_output_info.mm_height=mm_height;
        orig_output_info.num_crtcs=1;
        orig_output_info.num_modes=num_modes;
        orig_output_info.num_preferred=0;
        orig_output_info.num_clones=num_clones;
        const auto parentName=_xcb_randr_get_output_info_name(&origInfo); // not from our copy, because this function references variable-length fields
        orig_output_info.name_len=snprintf(nullptr, 0, "%*s~%d", origInfo.name_len, parentName, suffixIndex);
        name=static_cast<uint8_t*>(malloc(orig_output_info.name_len+1)); // newly-calculated length
        snprintf(reinterpret_cast<char*>(name), orig_output_info.name_len+1, "%*s~%d", origInfo.name_len, parentName, suffixIndex);
    }
    xcb_randr_get_output_info_reply_t* makeReturnValue() const
    {
        const auto begin=&orig_output_info;
        const auto end=_xcb_randr_get_output_info_name(begin)+orig_output_info.name_len;
        const auto size=reinterpret_cast<const char*>(end)-reinterpret_cast<const char*>(begin);
        const auto p=static_cast<xcb_randr_get_output_info_reply_t*>(malloc(size));
        // Copy fixed-size part
        *p=orig_output_info;
        // Now fill in the variable-length data
        *_xcb_randr_get_output_info_crtcs(p)=orig_output_info.crtc;
        std::copy_n(modes, orig_output_info.num_modes, _xcb_randr_get_output_info_modes(p));
        std::copy_n(clones, orig_output_info.num_clones, _xcb_randr_get_output_info_clones(p));
        std::copy_n(name, orig_output_info.name_len, _xcb_randr_get_output_info_name(p));
        return p;
    }
    ~FakeOutputInfo()
    {
        free(modes);
        free(clones);
    }
};

struct FakeModeInfo : xcb_randr_mode_info_t
{
    FakeModeInfo* nextInList=nullptr;
    char* name;

    FakeModeInfo(const uint32_t xid, xcb_randr_mode_info_t const& baseMode,
                 const uint16_t width, const uint16_t height)
        : xcb_randr_mode_info_t(baseMode)
    {
        id=xid;
        this->width = width;
        this->height = height;
        name_len = snprintf(nullptr, 0, "%dx%d", width, height);
        name=static_cast<char*>(malloc(name_len+1));
        snprintf(name, name_len+1, "%dx%d", width, height);
    }
    void setModeInfo(xcb_randr_mode_info_t const& info)
    {
        static_cast<xcb_randr_mode_info_t&>(*this) = info;
    }
    ~FakeModeInfo()
    {
        free(name);
    }
};

struct FakeScreenResources
{
    xcb_randr_get_screen_resources_reply_t* origRes;
    FakeCrtcInfo* fake_crtcs;
    FakeOutputInfo* fake_outputs;
    FakeModeInfo* fake_modes;

    FakeScreenResources(xcb_randr_get_screen_resources_reply_t const& originalResources,
                        FakeCrtcInfo* fake_crtcs, FakeOutputInfo* fake_outputs, FakeModeInfo* fake_modes)
        : fake_crtcs(fake_crtcs)
        , fake_outputs(fake_outputs)
        , fake_modes(fake_modes)
    {
        const auto begin=&originalResources;
        const auto end=_xcb_randr_get_screen_resources_names(begin)+originalResources.names_len;
        const auto size=reinterpret_cast<const char*>(end)-reinterpret_cast<const char*>(begin);
        origRes=static_cast<xcb_randr_get_screen_resources_reply_t*>(malloc(size));
        memcpy(origRes, &originalResources, size);
    }
    ~FakeScreenResources()
    {
        free(origRes);
        free_list(fake_crtcs);
        free_list(fake_outputs);
        free_list(fake_modes);
    }
    xcb_randr_get_screen_resources_reply_t* makeReturnValue()
    {
        unsigned fakeNamesTotalLen=0;
        for(auto* mode=fake_modes; mode; mode=mode->nextInList)
            fakeNamesTotalLen+=mode->name_len;
        const auto num_fake_crtcs=list_length(fake_crtcs);
        const auto num_fake_outputs=list_length(fake_outputs);
        const auto num_fake_modes=list_length(fake_modes);

        const auto begin=origRes;
        const auto end=_xcb_randr_get_screen_resources_names(begin)+origRes->names_len +
                        fakeNamesTotalLen+1 +
                        num_fake_crtcs*sizeof(xcb_randr_crtc_t) +
                        num_fake_outputs*sizeof(xcb_randr_output_t) +
                        num_fake_modes*sizeof(xcb_randr_mode_info_t);
        const auto size=reinterpret_cast<const char*>(end)-reinterpret_cast<const char*>(begin);
        const auto p=static_cast<xcb_randr_get_screen_resources_reply_t*>(malloc(size));
        // Copy fixed-size part
        *p=*origRes;

        // Now fill in the variable-length data
        const auto crtcsToFill=_xcb_randr_get_screen_resources_crtcs(p);
        const auto*const origCrtcs=_xcb_randr_get_screen_resources_crtcs(origRes);
        std::copy_n(origCrtcs, origRes->num_crtcs, crtcsToFill);
        unsigned i=origRes->num_crtcs;
        for(auto* crtc=fake_crtcs; crtc; crtc=crtc->nextInList)
        {
            crtcsToFill[i++]=crtc->xid;
            ++p->num_crtcs;
        }

        const auto outputsToFill=_xcb_randr_get_screen_resources_outputs(p);
        const auto*const origOutputs=_xcb_randr_get_screen_resources_outputs(origRes);
        std::copy_n(origOutputs, origRes->num_outputs, outputsToFill);
        i=origRes->num_outputs;
        for(auto* output=fake_outputs; output; output=output->nextInList)
        {
            outputsToFill[i++]=output->xid;
            ++p->num_outputs;
        }

        const auto modesToFill=_xcb_randr_get_screen_resources_modes(p);
        const auto*const origModes=_xcb_randr_get_screen_resources_modes(origRes);
        std::copy_n(origModes, origRes->num_modes, modesToFill);
        i=origRes->num_modes;
        for(auto* mode=fake_modes; mode; mode=mode->nextInList)
        {
            modesToFill[i++]=*mode;
            ++p->num_modes;
        }

        const auto namesToFill=_xcb_randr_get_screen_resources_names(p);
        const auto origNames=_xcb_randr_get_screen_resources_names(origRes);
        memcpy(namesToFill, origNames, origRes->names_len);
        namesToFill[origRes->names_len]=0;
        for(auto* mode=fake_modes; mode; mode=mode->nextInList)
            strcat(reinterpret_cast<char*>(namesToFill), mode->name);
        p->names_len=strlen(reinterpret_cast<const char*>(namesToFill));

        return p;
    }
};

uint32_t augmentXID(uint32_t xid, uint32_t n)
{
    return (xid & ~XID_SPLIT_MASK) | (n << XID_SPLIT_SHIFT);
}

/*
    Configuration management

    The configuration file format is documented in the management script. These
    functions load the configuration file and fill the FakeInfo lists with
    information on the fake outputs.
*/

char* _config_foreach_split(char* config, unsigned int* n, unsigned int x, unsigned int y, unsigned int width, unsigned int height,
                            xcb_randr_get_screen_resources_reply_t* resources, xcb_randr_output_t output,
                            xcb_randr_get_output_info_reply_t* output_info, xcb_randr_get_crtc_info_reply_t* crtc_info,
                            FakeCrtcInfo*** fake_crtcs, FakeOutputInfo*** fake_outputs, FakeModeInfo*** fake_modes)
{
    if(config[0] == 'N')
    {
        // Define a new output info
        **fake_outputs = newObj<FakeOutputInfo>(augmentXID(output, ++(*n)), output, *output_info,
                                                1, output_info->num_clones,
                                                output_info->mm_width * width / crtc_info->width,
                                                output_info->mm_height * height / crtc_info->height, *n);
        auto& fake_output_info = **fake_outputs;
        xcb_randr_output_t*const output_clones = _xcb_randr_get_output_info_clones(output_info);
        for(int i=0; i<fake_output_info->orig_output_info.num_clones; i++)
            fake_output_info->clones[i] = augmentXID(output_clones[i], *n);
        fake_output_info->orig_output_info.crtc = *fake_output_info->modes = augmentXID(output_info->crtc, *n);

        *fake_outputs = &(**fake_outputs)->nextInList;
        **fake_outputs = NULL;

        // Define a new CRTC info
        **fake_crtcs = newObj<FakeCrtcInfo>(augmentXID(output_info->crtc, *n), augmentXID(output, *n), *crtc_info,
                                            crtc_info->x + x, crtc_info->y + y, width, height, fake_output_info->modes[0]);
        *fake_crtcs = &(**fake_crtcs)->nextInList;
        **fake_crtcs = NULL;

        // Define a new fake mode
        const auto resources_modes = _xcb_randr_get_screen_resources_modes(resources);
        for(int i=0; i<resources->num_modes; i++)
        {
            if(resources_modes[i].id != crtc_info->mode)
                continue;

            **fake_modes = newObj<FakeModeInfo>(augmentXID(output_info->crtc, *n), resources_modes[i], width, height);
            *fake_modes = &(**fake_modes)->nextInList;
            **fake_modes = NULL;
            break;
        }

        return config + 1;
    }
    unsigned int split_pos = *(unsigned int *)&config[1];
    if(config[0] == 'H')
    {
        config = _config_foreach_split(config + 1 + 4, n, x, y, width, split_pos, resources, output, output_info, crtc_info,
                                       fake_crtcs, fake_outputs, fake_modes);
        return _config_foreach_split(config, n, x, y + split_pos, width, height - split_pos, resources, output, output_info, crtc_info,
                                     fake_crtcs, fake_outputs, fake_modes);
    }
    else
    {
        assert(config[0] == 'V');

        config = _config_foreach_split(config + 1 + 4, n, x, y, split_pos, height, resources, output, output_info, crtc_info,
                                       fake_crtcs, fake_outputs, fake_modes);
        return _config_foreach_split(config, n, x + split_pos, y, width - split_pos, height, resources, output, output_info, crtc_info,
                                     fake_crtcs, fake_outputs, fake_modes);
    }
}

int config_handle_output(xcb_connection_t* c, xcb_randr_get_screen_resources_reply_t* resources, xcb_randr_output_t output, char* target_edid,
                         FakeCrtcInfo*** fake_crtcs, FakeOutputInfo*** fake_outputs, FakeModeInfo*** fake_modes)
{
    for(char* config = config_file; (int)(config - config_file) <= (int)config_file_size; )
    {
        // Walk through the configuration file and search for the target_edid
        const auto size = *reinterpret_cast<unsigned*>(config);
        const char*const name = &config[4];
        const char*const edid = &config[4 + 128];
        const auto width = *reinterpret_cast<unsigned*>(&config[4 + 128 + 768]);
        const auto height = *reinterpret_cast<unsigned*>(&config[4 + 128 + 768 + 4]);
        const auto count = *reinterpret_cast<unsigned*>(&config[4 + 128 + 768 + 4 + 4]);

        if(strncmp(edid, target_edid, 768) == 0)
        {
            xcb_randr_get_output_info_cookie_t output_info_cookie = _xcb_randr_get_output_info(c, output, resources->config_timestamp);
            xcb_randr_get_output_info_reply_t* output_info = _xcb_randr_get_output_info_reply(c, output_info_cookie, NULL);
            if(!output_info) return 0;

            xcb_randr_get_crtc_info_cookie_t output_crtc_cookie = _xcb_randr_get_crtc_info(c, output_info->crtc, resources->config_timestamp);
            xcb_randr_get_crtc_info_reply_t* output_crtc = _xcb_randr_get_crtc_info_reply(c, output_crtc_cookie, NULL);
            if(!output_crtc) return 0;

            if(output_crtc->width == (int)width && output_crtc->height == (int)height)
            {
                // If it is found and the size matches, add fake outputs/crtcs to the list
                unsigned n = 0;
                _config_foreach_split(config + 4 + 128 + 768 + 4 + 4 + 4, &n, 0, 0, width, height, resources,
                                      output, output_info, output_crtc, fake_crtcs, fake_outputs, fake_modes);
                return 1;
            }

            free(output_info);
            free(output_crtc);
        }

        config += 4 + size;
    }

    return 0;
}

/*
    Helper function to return a hex-coded EDID string for a given output

    edid must point to a sufficiently large (768 bytes) buffer.
*/

int get_output_edid(xcb_connection_t* c, xcb_randr_output_t output, char* edid)
{
    xcb_intern_atom_cookie_t edid_atom_cookie = xcb_intern_atom(c, 1, 4, "EDID"); // 4 == strlen("EDID")
    xcb_intern_atom_reply_t* edid_atom = xcb_intern_atom_reply(c, edid_atom_cookie, NULL);
    if(!edid_atom) return 0;

    xcb_randr_get_output_property_cookie_t edid_prop_cookie = _xcb_randr_get_output_property(c, output, edid_atom->atom, 0, 0, 384, 0, 0);
    xcb_randr_get_output_property_reply_t* edid_prop = _xcb_randr_get_output_property_reply(c, edid_prop_cookie, NULL);
    if(!edid_prop) return 0;

    // EDID property is 8 bits (format = 8), according to protocol spec, num_items and xcb's length methods work equally
    const auto num_items = edid_prop->num_items;
    if(num_items > 0)
    {
        uint8_t* prop = _xcb_randr_get_output_property_data(edid_prop);

        for(int i=0; i < num_items; i++)
        {
            edid[2*i] = ((prop[i] >> 4) & 0xf) + '0';
            if(edid[2*i] > '9')
            {
                edid[2*i] += 'a' - '0' - 10;
            }

            edid[2*i+1] = (prop[i] & 0xf) + '0';
            if(edid[2*i+1] > '9')
            {
                edid[2*i+1] += 'a' - '0' - 10;
            }
        }
        edid[num_items*2] = 0; // (nms): overflow? what's going on with the edid buffer?

        free(edid_prop);
    }

    free(edid_atom);

    // (nms): why multiply by 2?  why num_items based return value at all?
    return num_items * 2;
}

FakeScreenResources* fakeScreenResources;
void updateFakeResources(xcb_connection_t* c, xcb_randr_get_screen_resources_reply_t* res, bool current)
{
    FakeOutputInfo* fake_outputs = NULL;
    FakeCrtcInfo* fake_crtcs = NULL;
    FakeModeInfo* fake_modes = NULL;

    FakeOutputInfo** fake_outputs_end = &fake_outputs;
    FakeCrtcInfo** fake_crtcs_end = &fake_crtcs;
    FakeModeInfo** fake_modes_end = &fake_modes;

    if(open_configuration()) return;

    xcb_randr_get_screen_resources_current_reply_t*const resc=(xcb_randr_get_screen_resources_current_reply_t*)res;
    xcb_randr_output_t*const res_outputs = current ? (xcb_randr_output_t*)_xcb_randr_get_screen_resources_current_outputs(resc)
                                                   :                      _xcb_randr_get_screen_resources_outputs(res);

    for(int i=0; i < res->num_outputs; ++i)
    {
        char output_edid[768];
        if(get_output_edid(c, res_outputs[i], output_edid) > 0)
            config_handle_output(c, res, res_outputs[i], output_edid, &fake_crtcs_end, &fake_outputs_end, &fake_modes_end);
    }

    fakeScreenResources = newObj<FakeScreenResources>(*res, fake_crtcs, fake_outputs, fake_modes);
}

void _init() __attribute__((constructor));
void _init()
{
    void* xrandr_lib = dlopen(REAL_LIB, RTLD_LAZY | RTLD_LOCAL);

    /*
        The following macro is defined by the skeleton header. It initializes
        static variables called _xcb_randr_ with references to the real
        xcb_randr_ functions.
        */
    FUNCTION_POINTER_INITIALIZATIONS;

    const auto randr_id=dlsym(xrandr_lib, "xcb_randr_id");
    memcpy(&xcb_randr_id, randr_id, sizeof xcb_randr_id);
}

} // namespace

/*
    Overridden library functions to add the fake output
*/

extern "C"
{
xcb_randr_get_screen_resources_current_reply_t* xcb_randr_get_screen_resources_current_reply(xcb_connection_t* c,
                                                                                             xcb_randr_get_screen_resources_current_cookie_t cookie,
                                                                                             xcb_generic_error_t** e)
{
    if(fakeScreenResources)
        deleteObj(fakeScreenResources);
    auto*const screen_resources = _xcb_randr_get_screen_resources_current_reply(c, cookie, e);
    updateFakeResources(c, reinterpret_cast<xcb_randr_get_screen_resources_reply_t*>(screen_resources), true);
    return reinterpret_cast<xcb_randr_get_screen_resources_current_reply_t*>(fakeScreenResources->makeReturnValue());
}
xcb_randr_get_screen_resources_reply_t* xcb_randr_get_screen_resources_reply(xcb_connection_t* c,
                                                                             xcb_randr_get_screen_resources_cookie_t cookie,
                                                                             xcb_generic_error_t** e)
{
    if(fakeScreenResources)
        deleteObj(fakeScreenResources);
    auto*const screen_resources = _xcb_randr_get_screen_resources_reply(c, cookie, e);
    updateFakeResources(c, screen_resources, false);
    return fakeScreenResources->makeReturnValue();
}

// --------------------- CRTC info ---------------------------
static AssocList<decltype(xcb_randr_get_crtc_info_cookie_t::sequence), xcb_randr_crtc_t> crtc_info_cookies;
xcb_randr_get_crtc_info_cookie_t xcb_randr_get_crtc_info(xcb_connection_t* c, xcb_randr_crtc_t crtc, xcb_timestamp_t config_timestamp)
{
    const auto cookie = _xcb_randr_get_crtc_info(c,crtc & ~XID_SPLIT_MASK, config_timestamp);
    crtc_info_cookies.insert(cookie.sequence,crtc);
    return cookie;
}
xcb_randr_get_crtc_info_cookie_t xcb_randr_get_crtc_info_unchecked(xcb_connection_t* c, xcb_randr_crtc_t crtc, xcb_timestamp_t config_timestamp)
{
    const auto cookie = _xcb_randr_get_crtc_info_unchecked(c,crtc & ~XID_SPLIT_MASK, config_timestamp);
    crtc_info_cookies.insert(cookie.sequence,crtc);
    return cookie;
}
xcb_randr_get_crtc_info_reply_t* xcb_randr_get_crtc_info_reply(xcb_connection_t* c, xcb_randr_get_crtc_info_cookie_t cookie, xcb_generic_error_t** e)
{
    const auto fakeCrtcItem=crtc_info_cookies.find(cookie.sequence);
    if(!fakeCrtcItem)
        return _xcb_randr_get_crtc_info_reply(c,cookie,e);

    const auto crtcId=fakeCrtcItem->data.value;
    crtc_info_cookies.erase(fakeCrtcItem);
    if(!fakeScreenResources) return nullptr; // FIXME: maybe call get_screen_resources{,_reply} instead of failing?
    if(!(crtcId & XID_SPLIT_MASK))
    {
        const auto info=_xcb_randr_get_crtc_info_reply(c,cookie,e);
        for(const auto* output=fakeScreenResources->fake_outputs; output; output=output->nextInList)
        {
            if((output->orig_output_info.crtc & ~XID_SPLIT_MASK)!=crtcId)
                continue;
            // This CRTC corresponds to a fake output. Hide its current mode.
            info->mode=0;
            info->x=info->y=info->width=info->height=0;
            break;
        }
        return info;
    }
    for(auto* crtc=fakeScreenResources->fake_crtcs; crtc; crtc=crtc->nextInList)
    {
        if(crtc->xid!=crtcId) continue;
        return crtc->makeReturnValue();
    }
    return nullptr;
}

// --------------------- Output info ---------------------------
static AssocList<decltype(xcb_randr_get_output_info_cookie_t::sequence), xcb_randr_output_t> output_info_cookies;
xcb_randr_get_output_info_cookie_t xcb_randr_get_output_info(xcb_connection_t* c, xcb_randr_output_t output, xcb_timestamp_t config_timestamp)
{
    const auto cookie = _xcb_randr_get_output_info(c,output & ~XID_SPLIT_MASK, config_timestamp);
    output_info_cookies.insert(cookie.sequence,output);
    return cookie;
}
xcb_randr_get_output_info_cookie_t xcb_randr_get_output_info_unchecked(xcb_connection_t* c, xcb_randr_output_t output, xcb_timestamp_t config_timestamp)
{
    const auto cookie = _xcb_randr_get_output_info_unchecked(c,output & ~XID_SPLIT_MASK, config_timestamp);
    output_info_cookies.insert(cookie.sequence,output);
    return cookie;
}
xcb_randr_get_output_info_reply_t* xcb_randr_get_output_info_reply(xcb_connection_t* c, xcb_randr_get_output_info_cookie_t cookie, xcb_generic_error_t** e)
{
    const auto fakeOutputItem=output_info_cookies.find(cookie.sequence);
    if(!fakeOutputItem)
        return _xcb_randr_get_output_info_reply(c,cookie,e);

    const auto outputId=fakeOutputItem->data.value;
    output_info_cookies.erase(fakeOutputItem);
    if(!fakeScreenResources) return nullptr; // FIXME: maybe call get_screen_resources{,_reply} instead of failing?
    if(!(outputId & XID_SPLIT_MASK))
    {
        const auto outputInfo=_xcb_randr_get_output_info_reply(c,cookie,e);
        if(xid_in_list(fakeScreenResources->fake_outputs, outputId))
        {
            // This output is fake. Make it look disconnected.
            outputInfo->connection=XCB_RANDR_CONNECTION_DISCONNECTED;
        }
        return outputInfo;
    }
    for(auto* output=fakeScreenResources->fake_outputs; output; output=output->nextInList)
    {
        if(output->xid!=outputId) continue;
        return output->makeReturnValue();
    }
    return nullptr;
}

xcb_extension_t xcb_randr_id;
} // extern "C"
