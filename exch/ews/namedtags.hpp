// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2022-2023 grommunio GmbH
// This file is part of Gromox.

#pragma once

#include <gromox/mapidefs.h>

static const PROPERTY_NAME NtCategories = {MNID_STRING, PS_PUBLIC_STRINGS, 0, const_cast<char*>("Keywords")};

static const PROPERTY_NAME NtAppointmentNotAllowPropose = {MNID_ID, PSETID_APPOINTMENT, PidLidAppointmentNotAllowPropose};
static const PROPERTY_NAME NtAppointmentRecur = {MNID_ID, PSETID_APPOINTMENT, PidLidAppointmentRecur};
static const PROPERTY_NAME NtAppointmentReplyTime = {MNID_ID, PSETID_APPOINTMENT, PidLidAppointmentReplyTime};
static const PROPERTY_NAME NtAppointmentSequence = {MNID_ID, PSETID_APPOINTMENT, PidLidAppointmentSequence};
static const PROPERTY_NAME NtAppointmentStateFlags = {MNID_ID, PSETID_APPOINTMENT, PidLidAppointmentStateFlags};
static const PROPERTY_NAME NtAppointmentSubType = {MNID_ID, PSETID_APPOINTMENT, PidLidAppointmentSubType};
static const PROPERTY_NAME NtBusyStatus = {MNID_ID, PSETID_APPOINTMENT, PidLidBusyStatus};
static const PROPERTY_NAME NtCommonEnd = {MNID_ID, PSETID_COMMON, PidLidCommonEnd};
static const PROPERTY_NAME NtCommonStart = {MNID_ID, PSETID_COMMON, PidLidCommonStart};
static const PROPERTY_NAME NtFInvited = {MNID_ID, PSETID_APPOINTMENT, PidLidFInvited};
static const PROPERTY_NAME NtGlobalObjectId = {MNID_ID, PSETID_MEETING, PidLidGlobalObjectId};
static const PROPERTY_NAME NtLocation = {MNID_ID, PSETID_APPOINTMENT, PidLidLocation};
static const PROPERTY_NAME NtResponseStatus = {MNID_ID, PSETID_APPOINTMENT, PidLidResponseStatus};
