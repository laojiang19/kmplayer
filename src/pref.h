/**
 * Copyright (C) 2003 Joonas Koivunen <rzei@mbnet.fi>
 * Copyright (C) 2003 Koos Vriezen <koos.vriezen@xs4all.nl>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef _KMPlayerPREF_H_
#define _KMPlayerPREF_H_

#include <kdialogbase.h>
#include <qframe.h>
#include <qmap.h>


class KMPlayerPrefGeneralPageGeneral; 	// general, general
class KMPlayerPrefSourcePageURL;        // source, url
class KMPlayerPrefRecordPage;           // recording
class RecorderPage;                     // base recorder
class KMPlayerPrefMEncoderPage;         // mencoder
class KMPlayerPrefMPlayerDumpstreamPage; // mplayer -dumpstream
class KMPlayerPrefFFMpegPage;           // ffmpeg
class KMPlayerPrefGeneralPageOutput;	// general, output
class KMPlayerPrefOPPageGeneral;	// OP = outputplugins, general
class KMPlayerPrefOPPagePostProc;	// outputplugins, postproc
class KMPlayer;
class KMPlayerSource;
class KMPlayerSettings;
class KMPlayerPreferencesPage;
class OutputDriver;
class QTabWidget;
class QTable;
class QGroupBox;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QRadioButton;
class QSlider;
class QSpinBox;
class QButtonGroup;
class KHistoryCombo;
class KComboBox;
class KURLRequester;


class KMPlayerPreferences : public KDialogBase
{
    Q_OBJECT
public:

    KMPlayerPreferences(KMPlayer *, KMPlayerSettings *);
    ~KMPlayerPreferences();

    KMPlayerPrefGeneralPageGeneral 	*m_GeneralPageGeneral;
    KMPlayerPrefSourcePageURL 		*m_SourcePageURL;
    KMPlayerPrefRecordPage 		*m_RecordPage;
    KMPlayerPrefMEncoderPage            *m_MEncoderPage;
    KMPlayerPrefMPlayerDumpstreamPage   *m_MPlayerDumpstreamPage;
    KMPlayerPrefFFMpegPage              *m_FFMpegPage;
    KMPlayerPrefGeneralPageOutput 	*m_GeneralPageOutput;
    KMPlayerPrefOPPageGeneral 		*m_OPPageGeneral;
    KMPlayerPrefOPPagePostProc		*m_OPPagePostproc;
    void setDefaults();
    void setPage (const char *);
    void addPrefPage (KMPlayerPreferencesPage *);
    void removePrefPage (KMPlayerPreferencesPage *);

    RecorderPage * recorders;
    QMap<QString, QTabWidget *> entries;
public slots:
    void confirmDefaults();
};

class KMPlayerPrefGeneralPageGeneral : public QFrame
{
    Q_OBJECT
public:
    KMPlayerPrefGeneralPageGeneral(QWidget *parent = 0);
    ~KMPlayerPrefGeneralPageGeneral() {}

    QCheckBox *keepSizeRatio;
    QCheckBox *loop;
    QCheckBox *showRecordButton;
    QCheckBox *showBroadcastButton;
    QCheckBox *framedrop;

    QSpinBox *seekTime;

};

class KMPlayerPrefSourcePageURL : public QFrame
{
    Q_OBJECT
public:
    KMPlayerPrefSourcePageURL (QWidget *parent);
    ~KMPlayerPrefSourcePageURL () {}

    KURLRequester * url;
    //KHistoryCombo * url;
    KComboBox * urllist;
    KURLRequester * sub_url;
    KComboBox * sub_urllist;
    QListBox * backend;
    QCheckBox * allowhref;
    bool changed;
private slots:
    void slotBrowse ();
    void slotTextChanged (const QString &);
};


class KMPlayerPrefRecordPage : public QFrame
{
    Q_OBJECT
public:
    KMPlayerPrefRecordPage (QWidget *parent, KMPlayer *, RecorderPage *);
    ~KMPlayerPrefRecordPage () {}

    KURLRequester * url;
    QButtonGroup * recorder;
    QButtonGroup * replay;
    QLineEdit * replaytime;
    QLabel * source;
public slots:
    void replayClicked (int id);
private slots:
    void slotRecord ();
    void sourceChanged (KMPlayerSource *);
    void recordingStarted ();
    void recordingFinished ();
private:
    KMPlayer * m_player;
    RecorderPage * m_recorders;
    QPushButton * recordButton;
};

class RecorderPage : public QFrame
{
    Q_OBJECT
public:
    RecorderPage (QWidget *parent, KMPlayer *);
    virtual ~RecorderPage () {};
    virtual void record () = 0;
    virtual QString name () = 0;
    virtual bool sourceSupported (KMPlayerSource *) = 0;
    RecorderPage * next;
protected:
    KMPlayer * m_player;
};

class KMPlayerPrefMEncoderPage : public RecorderPage 
{
    Q_OBJECT
public:
    KMPlayerPrefMEncoderPage (QWidget *parent, KMPlayer *);
    ~KMPlayerPrefMEncoderPage () {}

    void record ();
    QString name ();
    bool sourceSupported (KMPlayerSource *);

    QLineEdit * arguments;
    QButtonGroup * format;
public slots:
    void formatClicked (int id);
private:
};

class KMPlayerPrefMPlayerDumpstreamPage : public RecorderPage 
{
    Q_OBJECT
public:
    KMPlayerPrefMPlayerDumpstreamPage (QWidget *parent, KMPlayer *);
    ~KMPlayerPrefMPlayerDumpstreamPage () {}

    void record ();
    QString name ();
    bool sourceSupported (KMPlayerSource *);

    QLineEdit * arguments;
    QButtonGroup * format;
private:
};

class KMPlayerPrefFFMpegPage : public RecorderPage
{
    Q_OBJECT
public:
    KMPlayerPrefFFMpegPage (QWidget *parent, KMPlayer *);
    ~KMPlayerPrefFFMpegPage () {}

    void record ();
    QString name ();
    bool sourceSupported (KMPlayerSource *);

    QLineEdit * arguments;
    QButtonGroup * format;
private:
};


class KMPlayerPrefGeneralPageOutput : public QFrame
{
    Q_OBJECT
public:
    KMPlayerPrefGeneralPageOutput (QWidget *parent, OutputDriver * ad, OutputDriver * vd);
    ~KMPlayerPrefGeneralPageOutput() {}

    QListBox *videoDriver;
    QListBox *audioDriver;
};

class KMPlayerPrefOPPageGeneral : public QFrame
{
    Q_OBJECT
public:
    KMPlayerPrefOPPageGeneral(QWidget *parent = 0);
    ~KMPlayerPrefOPPageGeneral() {}
};

class KMPlayerPrefOPPagePostProc : public QFrame
{
    Q_OBJECT
public:
    KMPlayerPrefOPPagePostProc(QWidget *parent = 0);
    ~KMPlayerPrefOPPagePostProc() {}

    QCheckBox* postProcessing;
    QCheckBox* disablePPauto;
    QTabWidget* PostprocessingOptions;

    QRadioButton* defaultPreset;
    QRadioButton* customPreset;
    QRadioButton* fastPreset;

    QCheckBox* HzDeblockFilter;
    QCheckBox* VtDeblockFilter;
    QCheckBox* DeringFilter;
    QCheckBox* HzDeblockAQuality;
    QCheckBox* VtDeblockAQuality;
    QCheckBox* DeringAQuality;

    QCheckBox* AutolevelsFilter;
    QCheckBox* AutolevelsFullrange;
    QCheckBox* HzDeblockCFiltering;
    QCheckBox* VtDeblockCFiltering;
    QCheckBox* DeringCFiltering;
    QCheckBox* TmpNoiseFilter;
    QSlider* TmpNoiseSlider;

    QCheckBox* LinBlendDeinterlacer;
    QCheckBox* CubicIntDeinterlacer;
    QCheckBox* LinIntDeinterlacer;
    QCheckBox* MedianDeinterlacer;
    QCheckBox* FfmpegDeinterlacer;
};


#endif // _KMPlayerPREF_H_
