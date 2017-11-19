#!/usr/bin/env sgsh
#
# SYNOPSIS Highlight misspelled words
# DESCRIPTION
# Highlight the words that are misspelled in the command's standard
# input
# Demonstrates stream buffering.
#
#  Copyright 2013 Diomidis Spinellis
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#

scatter |{
	-| cat |>/stream/a
	-||>/stream/b
|} gather |{
	cat /stream/a
	cat /stream/b
|}
