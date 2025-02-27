# Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#     http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import paddle
from .public import *
from paddle.distributed.fleet.base.private_helper_function import wait_server_ready
from paddle.distributed.passes import new_pass, PassContext


class PsProgramBuilder(object):
    def __init__(self, pass_ctx):
        self.pass_ctx = pass_ctx
        self.attrs = self.pass_ctx._attrs
        self.loss = self.attrs['loss']
        self.cloned_main = self.attrs['cloned_main']
        self.cloned_startup = self.attrs['cloned_startup']

        self.use_ps_gpu = self.attrs['use_ps_gpu']
        self.use_heter_ps = self.attrs['is_heter_ps_mode']
        self.is_worker = self.attrs['is_worker']
        self.is_heter_worker = self.attrs['is_heter_worker']
        self.ps_mode = self.attrs['ps_mode']

        self.launch_barrier = self.attrs['launch_barrier']
        self.launch_barrier_flag = self.attrs['launch_barrier_flag']
        self.server_endpoints = self.attrs['role_maker']._get_pserver_endpoints(
        )

    def _optimize_programs(self):
        pass

    def _build_trainer_programs(self):
        raise NotImplementedError

    def _build_pserver_programs(self):
        is_sgd_adam = False
        ops = get_optimize_ops(self.attrs['origin_main_program'])
        if len(ops) == 0:
            return
        add_lr_decay_table_pass = new_pass('add_lr_decay_table_pass',
                                           self.attrs)
        add_lr_decay_table_pass.apply([], [], self.pass_ctx)
        for op in ops:
            if op.type in ["sgd", "adam"]:
                is_sgd_adam = True
                break
        if is_sgd_adam:
            return

    def _build_programs(self):
        if self.attrs['is_worker']:
            logger.info("start building trainer program")
            self._build_trainer_programs()
            fluid.framework.switch_startup_program(self.cloned_startup)
            self.loss.block.program = self.cloned_main

        elif self.attrs['is_server']:
            logger.info("start building pserver program")
            self._build_pserver_programs()
            self.loss.block.program = self.attrs['_main_server']
            fluid.framework.switch_startup_program(self.attrs[
                '_startup_server'])


class GeoPsProgramBuilder(PsProgramBuilder):  # 仅 CPU 模式
    def __init__(self, pass_ctx):
        logger.info("start building geo-ps program")
        super(GeoPsProgramBuilder, self).__init__(pass_ctx)
        if self.ps_mode != DistributedMode.GEO:
            raise ValueError("ps mode: {} not matched {}",
                             format(self.ps_mode, "GeoPsProgramBuilder"))

    def _build_trainer_programs(self):
        append_send_ops_pass = new_pass("append_send_ops_pass", self.attrs)
        append_send_ops_pass.apply([self.cloned_main], [None], self.pass_ctx)

        self.attrs['origin_main_program'] = self.cloned_main

        if self.launch_barrier and self.launch_barrier_flag:
            wait_server_ready(server_endpoints)

        return


class CpuSyncPsProgramBuilder(PsProgramBuilder):
    def __init__(self, pass_ctx):
        logger.info("start building cpu-sync-ps program")
        super(CpuSyncPsProgramBuilder, self).__init__(pass_ctx)
        if self.ps_mode != DistributedMode.SYNC and self.ps_mode != DistributedMode.ASYNC:
            raise ValueError("ps mode: {} not matched {}",
                             format(self.ps_mode, "CpuSyncPsProgramBuilder"))

    def _build_trainer_programs(self):
        add_lr_decay_table_pass = new_pass("add_lr_decay_table_pass",
                                           self.attrs)
        add_lr_decay_table_pass.apply([], [], self.pass_ctx)

        distributed_ops_pass = new_pass("distributed_ops_pass", self.attrs)
        distributed_ops_pass.apply([self.cloned_main], [None], self.pass_ctx)

        delete_optimizer_pass = new_pass("delete_optimizer_pass", self.attrs)
        delete_optimizer_pass.apply([self.cloned_main], [None], self.pass_ctx)

        append_send_ops_pass = new_pass("append_send_ops_pass", self.attrs)
        append_send_ops_pass.apply([self.cloned_main], [None], self.pass_ctx)

        delete_extra_optimizer_pass = new_pass("delete_extra_optimizer_pass",
                                               self.attrs)
        delete_extra_optimizer_pass.apply([self.attrs['origin_main_program']],
                                          [self.cloned_startup], self.pass_ctx)

        fake_init_ops_pass = new_pass("fake_init_ops_pass", self.attrs)
        fake_init_ops_pass.apply([None], [self.cloned_startup], self.pass_ctx)

        self.attrs['origin_main_program'] = self.cloned_main
        self.attrs['origin_startup_program'] = self.cloned_startup

        if self.launch_barrier and self.launch_barrier_flag:
            wait_server_ready(server_endpoints)

        return


class CpuAsyncPsProgramBuilder(CpuSyncPsProgramBuilder):
    def __init__(self, pass_ctx):
        logger.info("start building cpu-async-ps program")
        super(CpuAsyncPsProgramBuilder, self).__init__(pass_ctx)


class GpuPsProgramBuilder(PsProgramBuilder):
    def __init__(self, pass_ctx):
        logger.info("start building gpu-ps program")
        super(GpuPsProgramBuilder, self).__init__(pass_ctx)

    def _build_trainer_programs(self):

        add_lr_decay_table_pass = new_pass("add_lr_decay_table_pass",
                                           self.attrs)
        add_lr_decay_table_pass.apply([], [], self.pass_ctx)

        distributed_ops_pass = new_pass("distributed_ops_pass", self.attrs)
        distributed_ops_pass.apply([self.cloned_main], [None], self.pass_ctx)

        fake_init_ops_pass = new_pass("fake_init_ops_pass", self.attrs)
        fake_init_ops_pass.apply([None], [self.cloned_startup], self.pass_ctx)

        ps_gpu_pass = new_pass("ps_gpu_pass", self.attrs)
        ps_gpu_pass.apply([self.cloned_main], [None], self.pass_ctx)

        ps_transpile_pass = new_pass("ps_transpile_pass", self.attrs)
        ps_transpile_pass.apply([self.cloned_main], [self.cloned_startup],
                                self.pass_ctx)

        self.attrs['origin_main_program'] = self.cloned_main
        self.attrs['origin_startup_program'] = self.cloned_startup

        if self.launch_barrier and self.launch_barrier_flag:
            wait_server_ready(server_endpoints)

        return


class HeterAsyncPsProgramBuilder(PsProgramBuilder):
    def __init__(self, pass_ctx):
        logger.info("start building heter-async-ps program")
        super(HeterAsyncPsProgramBuilder, self).__init__(pass_ctx)
        if self.use_ps_gpu or self.ps_mode == DistributedMode.GEO or self.attrs[
                'is_heter_ps_mode'] == False:
            raise ValueError("ps mode: {} not matched {}",
                             format(self.ps_mode, "HeterAsyncPsProgramBuilder"))

    def _build_trainer_programs(self):
        add_lr_decay_table_pass = new_pass("add_lr_decay_table_pass",
                                           self.attrs)
        add_lr_decay_table_pass.apply([], [], self.pass_ctx)

        distributed_ops_pass = new_pass("distributed_ops_pass", self.attrs)
        distributed_ops_pass.apply([self.cloned_main], [None], self.pass_ctx)

        delete_optimizer_pass = new_pass("delete_optimizer_pass", self.attrs)
        delete_optimizer_pass.apply([self.cloned_main], [None], self.pass_ctx)

        append_send_ops_pass = new_pass("append_send_ops_pass", self.attrs)
        append_send_ops_pass.apply([self.cloned_main], [None], self.pass_ctx)

        delete_extra_optimizer_pass = new_pass("delete_extra_optimizer_pass",
                                               self.attrs)
        delete_extra_optimizer_pass.apply([self.attrs['origin_main_program']],
                                          [self.cloned_startup], self.pass_ctx)

        fake_init_ops_pass = new_pass("fake_init_ops_pass", self.attrs)
        fake_init_ops_pass.apply([None], [self.cloned_startup], self.pass_ctx)

        if self.is_heter_worker:
            split_heter_worker_ops_pass = new_pass(
                "split_heter_worker_ops_pass", self.attrs)
            split_heter_worker_ops_pass.apply([self.cloned_main], [None],
                                              self.pass_ctx)
        else:
            split_trainer_ops_pass = new_pass("split_trainer_ops_pass",
                                              self.attrs)
            split_trainer_ops_pass.apply([self.cloned_main], [None],
                                         self.pass_ctx)

        set_heter_pipeline_opt_pass = new_pass('set_heter_pipeline_opt_pass',
                                               self.attrs)
        set_heter_pipeline_opt_pass.apply([self.cloned_main],
                                          [self.cloned_startup], self.pass_ctx)

        if self.launch_barrier and self.launch_barrier_flag:
            wait_server_ready(server_endpoints)

        return

    def _build_programs(self):
        if self.attrs['is_worker'] or self.attrs['is_heter_worker']:
            self._build_trainer_programs()
            ps_set_heter_pipeline_opt_pass = new_pass(
                "set_heter_pipeline_opt_pass", self.attrs)
            ps_set_heter_pipeline_opt_pass.apply(
                [self.cloned_main], [self.cloned_startup], self.pass_ctx)

        elif self.attrs['is_server']:
            self._build_pserver_programs()
            self.loss.block.program = self.attrs['_main_server']
            fluid.framework.switch_startup_program(self.attrs[
                '_startup_server'])


class FlPsProgramBuilder(PsProgramBuilder):
    def __init__(self, pass_ctx):
        super(FlPsProgramBuilder, self).__init__(pass_ctx)

    def _build_trainer_programs(self):
        pass

    def _build_pserver_programs(self):
        pass

    def _build_programs(self):
        pass
