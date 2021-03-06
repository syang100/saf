#!/usr/bin/env python

# Copyright 2018 The SAF Authors. All Rights Reserved.
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

import  safpy

sp =  safpy.GetInstance()

camera1 = sp.Camera('GST_TEST')
detector1 = sp.Detector('cvsdk-ssd', 'person-detection-retail-0012')
camera2 = sp.Camera('GST_TEST')
detector2 = sp.Detector('cvsdk-ssd', 'person-detection-retail-0012')

graph = { "camera1" : [ "detector1:input0:output" ],
          "camera2" : [ "detector2:input0:output" ]}

sp.LoadGraph(graph)

sp.Start()

sp.Visualize(detector1, "output0")

sp.StopAndClean()
