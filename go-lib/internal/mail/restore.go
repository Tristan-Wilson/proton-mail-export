// Copyright (c) 2024 Proton AG
//
// This file is part of Proton Mail Bridge.
//
// Proton Mail Bridge is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Proton Mail Bridge is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Proton Mail Bridge. If not, see <https://www.gnu.org/licenses/>.

package mail

import (
	"context"
	"errors"
	"fmt"
	"path/filepath"
	"regexp"
	"time"

	"github.com/ProtonMail/export-tool/internal/apiclient"
	"github.com/ProtonMail/export-tool/internal/session"
	"github.com/ProtonMail/gopenpgp/v2/crypto"
	"github.com/sirupsen/logrus"
)

var mailFolderRegExp = regexp.MustCompile(`^mail_\d{8}_\d{6}$`)

type RestoreTask struct {
	ctx             context.Context
	startTime       time.Time
	ctxCancel       func()
	backupDir       string
	session         *session.Session
	log             *logrus.Entry
	labelMapping    map[string]string // map of [backup labelIDs] to remoteLabelIDs
	importLabelID   string
	importableCount int64
	importedCount   int64
	failedCount     int64
	cancelledByUser bool
}

func NewRestoreTask(ctx context.Context, backupDir string, session *session.Session) (*RestoreTask, error) {
	absPath, err := filepath.Abs(backupDir)
	if err != nil {
		return nil, err
	}

	log := logrus.WithField("backup", "mail").WithField("userID", session.GetUser().ID)

	ctx, cancel := context.WithCancel(ctx)

	return &RestoreTask{
		ctx:          ctx,
		ctxCancel:    cancel,
		backupDir:    absPath,
		session:      session,
		log:          log,
		labelMapping: make(map[string]string),
	}, nil
}

func (r *RestoreTask) Run(reporter Reporter) error {
	r.startTime = time.Now()
	defer func() { r.log.WithField("duration", time.Since(r.startTime)).Info("Finished") }()
	r.log.WithField("backupDir", r.backupDir).Info("Starting")

	messageInfoList, err := r.validateBackupDir(reporter)
	if err != nil {
		return err
	}
	r.log.WithField("messageCount", len(messageInfoList)).Info("Found messages to import")

	if err := r.restoreLabels(); err != nil {
		return err
	}

	if err := r.createImportLabel(); err != nil {
		return err
	}

	err = r.importMails(messageInfoList, reporter)

	r.log.WithFields(logrus.Fields{
		"importable": r.GetImportableCount(),
		"imported":   r.GetImportedCount(),
		"failed":     r.GetFailedCount(),
		"skipped":    r.GetSkippedCount(),
	}).Info("Report")

	return err
}

func (r *RestoreTask) Cancel() {
	r.cancelledByUser = true
	r.ctxCancel()
}

func (r *RestoreTask) Close() {
	// Nothing to do so far.
}

func (r *RestoreTask) GetBackupPath() string {
	return r.backupDir
}

func (r *RestoreTask) GetImportableCount() int64 {
	return r.importableCount
}

func (r *RestoreTask) GetImportedCount() int64 {
	return r.importedCount
}

func (r *RestoreTask) GetFailedCount() int64 {
	return r.failedCount
}

func (r *RestoreTask) GetSkippedCount() int64 {
	return r.importableCount - r.importedCount - r.failedCount
}

func (r *RestoreTask) GetOperationCancelledByUser() bool {
	return r.cancelledByUser
}

func (r *RestoreTask) withAddrKR(fn func(addrID string, addrKR *crypto.KeyRing) error) error {
	client := r.session.GetClient()
	addresses, err := client.GetAddresses(r.ctx)
	if err != nil {
		return err
	}

	if len(addresses) == 0 {
		return errors.New("address list is empty")
	}

	addrID := addresses[0].ID
	user := r.session.GetUser()
	salts := r.session.GetUserSalts()

	saltedKeyPass, err := salts.SaltForKey(r.session.GetMailboxPassword(), user.Keys.Primary().ID)
	if err != nil {
		return fmt.Errorf("failed to salt key password: %w", err)
	}

	if userKR, err := user.Keys.Unlock(saltedKeyPass, nil); err != nil {
		return fmt.Errorf("failed to unlock user keys: %w", err)
	} else if userKR.CountDecryptionEntities() == 0 {
		return fmt.Errorf("failed to unlock user keys")
	}

	unlockedKR, err := apiclient.NewUnlockedKeyRing(user, addresses, saltedKeyPass)
	if err != nil {
		return fmt.Errorf("failed to unlock user keyring:%w", err)
	}
	defer unlockedKR.Close()

	addrKR, ok := unlockedKR.GetAddrKeyRing(addresses[0].ID)
	if !ok {
		return fmt.Errorf("failed to get primary address keyring")
	}

	primaryKR, err := addrKR.FirstKey()
	if err != nil {
		return fmt.Errorf("failed to get primary key: %w", err)
	}

	return fn(addrID, primaryKR)
}
