W = '/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/'


def copy(src, dst):
    open(dst, 'w').write(open(W + src).read().replace('\r', ''))


def sub(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old[:60])
    open(path, 'w').write(s.replace(old, new))


# ---- 0322: work interval instances (work_interval_ctl operations 6-8) ----------------------------------------------
# iOS 16's libsystem_kernel reports each frame through work_interval_instance_start/update/finish, which call
# __work_interval_ctl with operations 6, 7 and 8 and a 56-byte record (shared cache 20H392: the work interval ID,
# the instance ID, start, finish, deadline, complexity, the create flags). The open-source xnu-8796 has no such
# operations: post17's kernel answered ENOTSUP to backboardd and mediaserverd for every frame, so the performance
# controller never saw a frame. The 20H392 iPhone10,3 kernelcache's work_interval_ctl (syscall 499, a jump table
# over operations 2-11) handles them as below: the kernel checks the thread's work interval, the machine layer
# builds a perfcontrol_work_interval_instance (the structure and the work_interval_ctl callout are in the open
# source) and calls the performance controller, which names the instance at its start.
sub('bsd/sys/work_interval.h',
    '#define WORK_INTERVAL_OPERATION_SET_WORKLOAD_ID  0x0000000b    /* arg is a work_interval_workload_id_params */\n',
    '#define WORK_INTERVAL_OPERATION_SET_WORKLOAD_ID  0x0000000b    /* arg is a work_interval_workload_id_params */\n'
    '#if defined(PANDORA)\n'
    '/* 0322: the instance operations of iOS 16 (libsystem_kernel work_interval_instance_*), arg a\n'
    ' * work_interval_instance_params; left out of the open-source release. */\n'
    '#define WORK_INTERVAL_OPERATION_INSTANCE_START   0x00000006\n'
    '#define WORK_INTERVAL_OPERATION_INSTANCE_UPDATE  0x00000007\n'
    '#define WORK_INTERVAL_OPERATION_INSTANCE_FINISH  0x00000008\n'
    '#endif\n')
sub('bsd/sys/work_interval.h',
    'struct work_interval_create_params {\n',
    '#if defined(PANDORA)\n'
    '/* 0322: libsystem_kernel\'s 56-byte instance (work_interval_instance_alloc), passed whole to operations 6-8. */\n'
    'struct work_interval_instance_params {\n'
    '\tuint64_t        wiip_work_interval_id;\n'
    '\tuint64_t        wiip_instance_id;       /* out at start; update and finish need it */\n'
    '\tuint64_t        wiip_start;\n'
    '\tuint64_t        wiip_finish;\n'
    '\tuint64_t        wiip_deadline;\n'
    '\tuint64_t        wiip_complexity;\n'
    '\tuint32_t        wiip_create_flags;\n'
    '\tuint32_t        wiip_reserved;\n'
    '};\n'
    '#endif\n\n'
    'struct work_interval_create_params {\n')

sub('osfmk/kern/work_interval.h',
    'extern kern_return_t\nkern_work_interval_notify(thread_t thread, struct kern_work_interval_args* kwi_args);\n',
    'extern kern_return_t\nkern_work_interval_notify(thread_t thread, struct kern_work_interval_args* kwi_args);\n'
    '#if defined(PANDORA)\n'
    '/* 0322: an instance start (0), update (1) or finish (2), as work_interval_ctl_t. */\n'
    'struct kern_work_interval_instance_args {\n'
    '\tuint64_t work_interval_id;\n'
    '\tuint64_t instance_id;\n'
    '\tuint64_t start;\n'
    '\tuint64_t finish;\n'
    '\tuint64_t deadline;\n'
    '\tuint64_t complexity;\n'
    '\tuint32_t create_flags;\n'
    '};\n'
    'extern kern_return_t\n'
    'kern_work_interval_instance_ctl(thread_t thread, uint32_t ctl, struct kern_work_interval_instance_args *args);\n'
    '#endif\n')

sub('osfmk/kern/machine.h',
    'extern void machine_work_interval_notify(thread_t thread, struct kern_work_interval_args* kwi_args);\n',
    'extern void machine_work_interval_notify(thread_t thread, struct kern_work_interval_args* kwi_args);\n'
    '#if defined(PANDORA)\n'
    'struct kern_work_interval_instance_args;\n'
    'extern void machine_work_interval_instance_ctl(thread_t thread, uint32_t ctl,\n'
    '    struct kern_work_interval_instance_args *args);   /* osfmk/arm/pandora_perfctl.c (0322) */\n'
    '#endif\n')

sub('osfmk/kern/work_interval.c',
    '/* Start at 1, 0 is not a valid work interval ID */\n',
    '#if defined(PANDORA)\n'
    '/*\n'
    ' * 0322: a work interval instance, as the 20H392 iPhone10,3 kernelcache does it: the calling thread must have\n'
    ' * joined the work interval; an ignored one (WORK_INTERVAL_FLAG_IGNORED) succeeds without a callout; a\n'
    ' * complexity is refused where the workload has an ID without WORK_INTERVAL_WORKLOAD_ID_COMPLEXITY_ALLOWED.\n'
    ' * (Apple\'s also fails on one thread flag this source does not show; not modeled.)\n'
    ' */\n'
    'kern_return_t\n'
    'kern_work_interval_instance_ctl(thread_t thread, uint32_t ctl, struct kern_work_interval_instance_args *args)\n'
    '{\n'
    '\tassert(thread == current_thread());\n'
    '\n'
    '\tstruct work_interval *work_interval = thread->th_work_interval;\n'
    '\tif (args->work_interval_id == 0 || work_interval == NULL ||\n'
    '\t    work_interval->wi_id != args->work_interval_id) {\n'
    '\t\treturn KERN_INVALID_ARGUMENT;\n'
    '\t}\n'
    '\tif (args->create_flags & WORK_INTERVAL_FLAG_IGNORED) {\n'
    '\t\treturn KERN_SUCCESS;\n'
    '\t}\n'
    '\tif (args->complexity != 0 &&\n'
    '\t    (work_interval->wi_wlid_flags & (WORK_INTERVAL_WORKLOAD_ID_COMPLEXITY_ALLOWED | WORK_INTERVAL_WORKLOAD_ID_HAS_ID)) ==\n'
    '\t    WORK_INTERVAL_WORKLOAD_ID_HAS_ID) {\n'
    '\t\treturn KERN_INVALID_ARGUMENT;\n'
    '\t}\n'
    '\tmachine_work_interval_instance_ctl(thread, ctl, args);\n'
    '\treturn KERN_SUCCESS;\n'
    '}\n'
    '#endif /* PANDORA */\n'
    '\n'
    '/* Start at 1, 0 is not a valid work interval ID */\n')

sub('bsd/kern/sys_work_interval.c',
    '\t\tbreak;\n\n\tdefault:\n\t\treturn ENOTSUP;\n\t}\n',
    '\t\tbreak;\n\n'
    '#if defined(PANDORA)\n'
    '\tcase WORK_INTERVAL_OPERATION_INSTANCE_START:\n'
    '\tcase WORK_INTERVAL_OPERATION_INSTANCE_UPDATE:\n'
    '\tcase WORK_INTERVAL_OPERATION_INSTANCE_FINISH: {\n'
    '\t\t/* 0322: as the 20H392 kernelcache: an update or finish of an instance never started is a no-op; a start\n'
    '\t\t * copies the instance back with the ID the performance controller gave it. */\n'
    '\t\tstruct work_interval_instance_params instance;\n'
    '\t\tif (uap->arg == USER_ADDR_NULL || uap->work_interval_id == 0 || uap->len < sizeof(instance)) {\n'
    '\t\t\treturn EINVAL;\n'
    '\t\t}\n'
    '\t\tif ((error = copyin(uap->arg, &instance, sizeof(instance)))) {\n'
    '\t\t\treturn error;\n'
    '\t\t}\n'
    '\t\tconst bool start = operation == WORK_INTERVAL_OPERATION_INSTANCE_START;\n'
    '\t\tif (!start && instance.wiip_instance_id == 0) {\n'
    '\t\t\tbreak;\n'
    '\t\t}\n'
    '\t\tstruct kern_work_interval_instance_args instance_args = {\n'
    '\t\t\t.work_interval_id = uap->work_interval_id,\n'
    '\t\t\t.instance_id = instance.wiip_instance_id,\n'
    '\t\t\t.start = instance.wiip_start,\n'
    '\t\t\t.finish = instance.wiip_finish,\n'
    '\t\t\t.deadline = instance.wiip_deadline,\n'
    '\t\t\t.complexity = instance.wiip_complexity,\n'
    '\t\t\t.create_flags = instance.wiip_create_flags,\n'
    '\t\t};\n'
    '\t\tkret = kern_work_interval_instance_ctl(current_thread(),\n'
    '\t\t    operation - WORK_INTERVAL_OPERATION_INSTANCE_START, &instance_args);\n'
    '\t\tif (kret != KERN_SUCCESS) {\n'
    '\t\t\treturn EINVAL;\n'
    '\t\t}\n'
    '\t\tif (start) {\n'
    '\t\t\tinstance.wiip_instance_id = instance_args.instance_id;\n'
    '\t\t\terror = copyout(&instance, uap->arg, sizeof(instance));\n'
    '\t\t}\n'
    '\t\tbreak;\n'
    '\t}\n'
    '#endif /* PANDORA */\n\n'
    '\tdefault:\n\t\treturn ENOTSUP;\n\t}\n')

copy('pandora_perfctl.c', 'osfmk/arm/pandora_perfctl.c')
