#!/usr/bin/env python3
"""Create the Vulkan pipeline a metal2vulkan meta file (--emit-meta) describes, on the first
Vulkan device, and report whether the driver accepted it.

  vk_pipeline.py META.json [META.json ...]

Meant for Turnip on freedreno's noop drm-shim (FD_CHIP_ID selects the GPU): creating the
pipeline makes Turnip compile the SPIR-V to that GPU's machine code, nothing is executed.
Prints one line per meta file: "PASS name" or "FAIL name: reason".
"""
import json
import os
import sys

import vulkan as vk

VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT = 0x1
VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT = 1000342000
VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR = 1000232000
CAPTURE_STATISTICS = 0x40  # VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR


def make_device():
    app = vk.VkApplicationInfo(sType=vk.VK_STRUCTURE_TYPE_APPLICATION_INFO, pApplicationName='m2v',
                               apiVersion=vk.VK_MAKE_VERSION(1, 3, 0))
    inst = vk.vkCreateInstance(vk.VkInstanceCreateInfo(sType=vk.VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                       pApplicationInfo=app), None)
    phys = vk.vkEnumeratePhysicalDevices(inst)[0]
    props = vk.vkGetPhysicalDeviceProperties(phys)
    exts = {e.extensionName for e in vk.vkEnumerateDeviceExtensionProperties(phys, None)}
    want = [e for e in ('VK_EXT_rasterization_order_attachment_access',
                        'VK_KHR_dynamic_rendering_local_read',
                        'VK_KHR_pipeline_executable_properties') if e in exts]
    # Enable every core 1.0-1.3 feature the device reports (query, then pass the same chain back).
    # Statistics (VK_KHR_pipeline_executable_properties) prove the driver compiled the shaders.
    pexec = vk.VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR(
        sType=vk.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
        pipelineExecutableInfo=vk.VK_TRUE) if 'VK_KHR_pipeline_executable_properties' in want else None
    feats13 = vk.VkPhysicalDeviceVulkan13Features(sType=vk.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                  pNext=pexec)
    feats12 = vk.VkPhysicalDeviceVulkan12Features(sType=vk.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                  pNext=feats13)
    feats11 = vk.VkPhysicalDeviceVulkan11Features(sType=vk.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                                  pNext=feats12)
    feats2 = vk.VkPhysicalDeviceFeatures2(sType=vk.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, pNext=feats11)
    vk.vkGetPhysicalDeviceFeatures2(phys, feats2)
    queue = vk.VkDeviceQueueCreateInfo(sType=vk.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                       queueFamilyIndex=0, queueCount=1, pQueuePriorities=[1.0])
    dev = vk.vkCreateDevice(phys, vk.VkDeviceCreateInfo(
        sType=vk.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, pNext=feats2, queueCreateInfoCount=1,
        pQueueCreateInfos=[queue], enabledExtensionCount=len(want), ppEnabledExtensionNames=want), None)
    return inst, dev, props, want


def spec_info(stage):
    consts = stage.get('specialization', {}).get('constants', [])
    if not consts:
        return None
    entries, data, off = [], b'', 0
    for c in consts:
        size = c.get('size', 4)
        entries.append(vk.VkSpecializationMapEntry(constantID=c['id'], offset=off, size=size))
        data += int(c.get('default', 0)).to_bytes(size, 'little')
        off += size
    return vk.VkSpecializationInfo(mapEntryCount=len(entries), pMapEntries=entries,
                                   dataSize=len(data), pData=vk.ffi.from_buffer(data))


STAGE_BITS = {'vert': vk.VK_SHADER_STAGE_VERTEX_BIT, 'frag': vk.VK_SHADER_STAGE_FRAGMENT_BIT,
              'comp': vk.VK_SHADER_STAGE_COMPUTE_BIT}


def build(dev, meta_path, exts):
    m = json.load(open(meta_path))
    keep = []
    set_layouts = []
    for s in sorted(m['layout']['sets'], key=lambda s: s['set']):
        while len(set_layouts) < s['set']:
            set_layouts.append(vk.vkCreateDescriptorSetLayout(dev, vk.VkDescriptorSetLayoutCreateInfo(
                sType=vk.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, bindingCount=0), None))
        binds = [vk.VkDescriptorSetLayoutBinding(binding=b['binding'], descriptorType=b['ty'],
                                                 descriptorCount=b['count'], stageFlags=b['stages'])
                 for b in s['bindings']]
        set_layouts.append(vk.vkCreateDescriptorSetLayout(dev, vk.VkDescriptorSetLayoutCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, flags=s.get('flags', 0),
            bindingCount=len(binds), pBindings=binds), None))
    pcs = [vk.VkPushConstantRange(stageFlags=p['stages'], offset=p['offset'], size=p['size'])
           for p in m['layout'].get('push_constants', [])]
    layout = vk.vkCreatePipelineLayout(dev, vk.VkPipelineLayoutCreateInfo(
        sType=vk.VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, setLayoutCount=len(set_layouts),
        pSetLayouts=set_layouts or None, pushConstantRangeCount=len(pcs), pPushConstantRanges=pcs or None), None)
    stages = []
    for st in m['stages']:
        code = open(st['spirv'], 'rb').read()
        mod = vk.vkCreateShaderModule(dev, vk.VkShaderModuleCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, codeSize=len(code), pCode=code), None)
        si = spec_info(st)
        keep.append(si)
        stages.append(vk.VkPipelineShaderStageCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, stage=STAGE_BITS[st['stage']],
            module=mod, pName=st.get('entry_point', 'main'), pSpecializationInfo=si))
    if m['kind'] == 'compute':
        return vk.vkCreateComputePipelines(dev, None, 1, [vk.VkComputePipelineCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, flags=CAPTURE_STATISTICS,
            stage=stages[0], layout=layout)], None)[0]
    g = m['graphics']
    vi = m.get('vertex_input') or {'bindings': [], 'attributes': []}
    vbind = [vk.VkVertexInputBindingDescription(binding=b['binding'], stride=b['stride'].get('Static', 0)
                                                if isinstance(b['stride'], dict) else b['stride'],
                                                inputRate=b['input_rate']) for b in vi['bindings']]
    vattr = [vk.VkVertexInputAttributeDescription(location=a['location'], binding=a['binding'],
                                                  format=a['format'], offset=a['offset'])
             for a in vi['attributes']]
    cols = g['color_attachments']
    formats = [c['format'] if c else vk.VK_FORMAT_UNDEFINED for c in cols]
    blends = [vk.VkPipelineColorBlendAttachmentState(blendEnable=vk.VK_FALSE,
                                                     colorWriteMask=(c or {}).get('write_mask', 15))
              for c in cols]
    rorder = bool(g.get('rasterization_order_attachment_access'))
    rendering = vk.VkPipelineRenderingCreateInfo(
        sType=vk.VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, colorAttachmentCount=len(formats),
        pColorAttachmentFormats=formats, depthAttachmentFormat=g.get('depth_format') or 0,
        stencilAttachmentFormat=g.get('stencil_format') or 0)
    dyn = [vk.VK_DYNAMIC_STATE_VIEWPORT, vk.VK_DYNAMIC_STATE_SCISSOR]
    info = vk.VkGraphicsPipelineCreateInfo(
        sType=vk.VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, pNext=rendering, flags=CAPTURE_STATISTICS,
        stageCount=len(stages), pStages=stages,
        pVertexInputState=vk.VkPipelineVertexInputStateCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            vertexBindingDescriptionCount=len(vbind), pVertexBindingDescriptions=vbind or None,
            vertexAttributeDescriptionCount=len(vattr), pVertexAttributeDescriptions=vattr or None),
        pInputAssemblyState=vk.VkPipelineInputAssemblyStateCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, topology=g['topology']),
        pViewportState=vk.VkPipelineViewportStateCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, viewportCount=1, scissorCount=1),
        pRasterizationState=vk.VkPipelineRasterizationStateCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            rasterizerDiscardEnable=vk.VK_TRUE if g.get('rasterizer_discard') else vk.VK_FALSE,
            polygonMode=vk.VK_POLYGON_MODE_FILL, cullMode=0, frontFace=0, lineWidth=1.0),
        pMultisampleState=vk.VkPipelineMultisampleStateCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            rasterizationSamples=g.get('rasterization_samples', 1)),
        pColorBlendState=vk.VkPipelineColorBlendStateCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            flags=VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT if rorder else 0,
            attachmentCount=len(blends), pAttachments=blends or None),
        pDynamicState=vk.VkPipelineDynamicStateCreateInfo(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, dynamicStateCount=len(dyn),
            pDynamicStates=dyn),
        layout=layout)
    return vk.vkCreateGraphicsPipelines(dev, None, 1, [info], None)[0]


def statistics(dev, pipeline):
    """Per stage: the driver's name for it and its statistics (instruction count, registers...)."""
    get_props = vk.vkGetDeviceProcAddr(dev, 'vkGetPipelineExecutablePropertiesKHR')
    get_stats = vk.vkGetDeviceProcAddr(dev, 'vkGetPipelineExecutableStatisticsKHR')
    info = vk.VkPipelineInfoKHR(sType=vk.VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, pipeline=pipeline)
    out = []
    for i, ex in enumerate(get_props(dev, info)):
        stats = get_stats(dev, vk.VkPipelineExecutableInfoKHR(
            sType=vk.VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, pipeline=pipeline, executableIndex=i))
        vals = {}
        for st in stats:
            v = st.value
            vals[vk.ffi.string(st.name).decode() if not isinstance(st.name, str) else st.name] = (
                v.u64 if st.format == vk.VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR else
                v.i64 if st.format == vk.VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR else
                v.f64 if st.format == vk.VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR else bool(v.b32))
        name = ex.name if isinstance(ex.name, str) else vk.ffi.string(ex.name).decode()
        out.append((name, vals))
    return out


def main():
    inst, dev, props, exts = make_device()
    print(f'# device: {props.deviceName} (api {props.apiVersion >> 22}.{(props.apiVersion >> 12) & 0x3ff}), '
          f'extensions enabled: {exts}', flush=True)
    for path in sys.argv[1:]:
        name = os.path.basename(path)
        try:
            pipeline = build(dev, path, exts)
            summary = ''
            if 'VK_KHR_pipeline_executable_properties' in exts:
                parts = []
                for stage, vals in statistics(dev, pipeline):
                    keys = [k for k in vals if 'nstruction' in k] or list(vals)[:1]
                    parts.append(f"{stage}: {', '.join(f'{k}={vals[k]}' for k in keys[:2])}")
                summary = ' | ' + '; '.join(parts)
            print(f'PASS {name}{summary}', flush=True)
        except Exception as e:  # driver errors arrive as vk exceptions
            print(f'FAIL {name}: {type(e).__name__} {e}', flush=True)


if __name__ == '__main__':
    main()
