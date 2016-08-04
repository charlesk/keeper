/*
 * Copyright 2013-2016 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Ted Gould <ted.gould@canonical.com>
 *     Xavi Garcia <xavi.garcia.mena@gmail.com>
 *     Charles Kerr <charles.kerr@canonical.com>
 */

#include <future>
#include <thread>

#include <QCoreApplication>
#include <QSignalSpy>

#include <gtest/gtest.h>
#include <gio/gio.h>
#include <ubuntu-app-launch/registry.h>
#include <libdbustest/dbus-test.h>

#include <libqtdbustest/DBusTestRunner.h>
#include <libqtdbustest/QProcessDBusService.h>
#include <libqtdbusmock/DBusMock.h>

#include "mir-mock.h"
#include <helper/backup-helper.h>
#include <qdbus-stubs/dbus-types.h>
#include "qdbus-stubs/keeper_user_interface.h"

#include "tests/fakes/fake-backup-helper.h"
#include "tests/utils/file-utils.h"
#include "tests/utils/xdg-user-dirs-sandbox.h"
#include "../../../src/service/app-const.h"


namespace
{
constexpr char const UPSTART_PATH[] = "/com/ubuntu/Upstart";
constexpr char const UPSTART_INTERFACE[] = "com.ubuntu.Upstart0_6";
constexpr char const UPSTART_INSTANCE[] = "com.ubuntu.Upstart0_6.Instance";
constexpr char const UPSTART_JOB[] = "com.ubuntu.Upstart0_6.Job";
constexpr char const UNTRUSTED_HELPER_PATH[] = "/com/test/untrusted/helper";
}

extern "C" {
    #include <ubuntu-app-launch.h>
    #include <fcntl.h>
}

class TestHelpers : public ::testing::Test
{
public:
    TestHelpers() = default;

    ~TestHelpers() = default;

protected:
    DbusTestService* service = nullptr;
    DbusTestDbusMock* mock = nullptr;
    DbusTestDbusMock* cgmock = nullptr;
    GDBusConnection* bus = nullptr;
    std::string last_focus_appid;
    std::string last_resume_appid;
    guint resume_timeout = 0;
    std::shared_ptr<ubuntu::app_launch::Registry> registry;
    DbusTestProcess * keeper = nullptr;
    QProcess keeper_client;
    QTemporaryDir xdg_data_home_dir;

private:
    static void focus_cb(const gchar* appid, gpointer user_data)
    {
        g_debug("Focus Callback: %s", appid);
        auto _this = static_cast<TestHelpers*>(user_data);
        _this->last_focus_appid = appid;
    }

    static void resume_cb(const gchar* appid, gpointer user_data)
    {
        g_debug("Resume Callback: %s", appid);
        auto _this = static_cast<TestHelpers*>(user_data);
        _this->last_resume_appid = appid;

        if (_this->resume_timeout > 0)
        {
            _this->pause(_this->resume_timeout);
        }
    }

protected:
    /* Useful debugging stuff, but not on by default.  You really want to
       not get all this noise typically */
    void debugConnection()
    {
        if (true)
        {
            return;
        }

        DbusTestBustle* bustle = dbus_test_bustle_new("test.bustle");
        dbus_test_service_add_task(service, DBUS_TEST_TASK(bustle));
        g_object_unref(bustle);

        DbusTestProcess* monitor = dbus_test_process_new("dbus-monitor");
        dbus_test_service_add_task(service, DBUS_TEST_TASK(monitor));
        g_object_unref(monitor);
    }

    void startTasks()
    {
        dbus_test_service_start_tasks(service);

        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        g_dbus_connection_set_exit_on_close(bus, FALSE);
        g_object_add_weak_pointer(G_OBJECT(bus), (gpointer*)&bus);

        /* Make sure we pretend the CG manager is just on our bus */
        g_setenv("UBUNTU_APP_LAUNCH_CG_MANAGER_SESSION_BUS", "YES", TRUE);

        ASSERT_TRUE(ubuntu_app_launch_observer_add_app_focus(focus_cb, this));
        ASSERT_TRUE(ubuntu_app_launch_observer_add_app_resume(resume_cb, this));

        registry = std::make_shared<ubuntu::app_launch::Registry>();
    }

    virtual void SetUp() override
    {
        Helper::registerMetaTypes();

        // storage framework uses XDG_DATA_HOME to create the
        // folder where all its uploaded files will be placed.
        // we remove the temporary directory ourselves to be able to check the
        // contents in case of error
        xdg_data_home_dir.setAutoRemove(false);

        /* Click DB test mode */
        g_setenv("TEST_CLICK_DB", "click-db-dir", TRUE);
        g_setenv("TEST_CLICK_USER", "test-user", TRUE);

        gchar* linkfarmpath = g_build_filename(CMAKE_SOURCE_DIR, "link-farm", NULL);
        g_setenv("UBUNTU_APP_LAUNCH_LINK_FARM", linkfarmpath, TRUE);
        g_free(linkfarmpath);

        g_setenv("XDG_DATA_DIRS", CMAKE_SOURCE_DIR, TRUE);
        g_setenv("XDG_CACHE_HOME", CMAKE_SOURCE_DIR "/libertine-data", TRUE);
        g_setenv("XDG_DATA_HOME", xdg_data_home_dir.path().toLatin1().data(), TRUE);

        qDebug() << "XDG_DATA_HOME ON SETUP is: " << xdg_data_home_dir.path();

        service = dbus_test_service_new(NULL);

        keeper = dbus_test_process_new(KEEPER_SERVICE_BIN);
        dbus_test_task_set_name(DBUS_TEST_TASK(keeper), "Keeper");
        dbus_test_service_add_task(service, DBUS_TEST_TASK(keeper));

        debugConnection();

        mock = dbus_test_dbus_mock_new("com.ubuntu.Upstart");

        DbusTestDbusMockObject* obj =
            dbus_test_dbus_mock_get_object(mock, UPSTART_PATH, UPSTART_INTERFACE, NULL);

        dbus_test_dbus_mock_object_add_method(mock, obj, "EmitEvent", G_VARIANT_TYPE("(sasb)"), NULL, "", NULL);

        dbus_test_dbus_mock_object_add_method(mock, obj, "GetJobByName", G_VARIANT_TYPE("s"), G_VARIANT_TYPE("o"),
                                              "if args[0] == 'application-click':\n"
                                              "	ret = dbus.ObjectPath('/com/test/application_click')\n"
                                              "elif args[0] == 'application-legacy':\n"
                                              "	ret = dbus.ObjectPath('/com/test/application_legacy')\n"
                                              "elif args[0] == 'untrusted-helper':\n"
                                              "	ret = dbus.ObjectPath('/com/test/untrusted/helper')\n",
                                              NULL);

        dbus_test_dbus_mock_object_add_method(mock, obj, "SetEnv", G_VARIANT_TYPE("(assb)"), NULL, "", NULL);

        /* Click App */
        DbusTestDbusMockObject* jobobj =
            dbus_test_dbus_mock_get_object(mock, "/com/test/application_click", UPSTART_JOB, NULL);

        dbus_test_dbus_mock_object_add_method(
            mock, jobobj, "Start", G_VARIANT_TYPE("(asb)"), NULL,
            "if args[0][0] == 'APP_ID=com.test.good_application_1.2.3':"
            "    raise dbus.exceptions.DBusException('Foo running', name='com.ubuntu.Upstart0_6.Error.AlreadyStarted')",
            NULL);

        dbus_test_dbus_mock_object_add_method(mock, jobobj, "Stop", G_VARIANT_TYPE("(asb)"), NULL, "", NULL);

        dbus_test_dbus_mock_object_add_method(mock, jobobj, "GetAllInstances", NULL, G_VARIANT_TYPE("ao"),
                                              "ret = [ dbus.ObjectPath('/com/test/app_instance') ]", NULL);

        DbusTestDbusMockObject* instobj =
            dbus_test_dbus_mock_get_object(mock, "/com/test/app_instance", UPSTART_INSTANCE, NULL);
        dbus_test_dbus_mock_object_add_property(mock, instobj, "name", G_VARIANT_TYPE_STRING,
                                                g_variant_new_string("com.test.good_application_1.2.3"), NULL);
        gchar* process_var = g_strdup_printf("[('main', %d)]", getpid());
        dbus_test_dbus_mock_object_add_property(mock, instobj, "processes", G_VARIANT_TYPE("a(si)"),
                                                g_variant_new_parsed(process_var), NULL);
        g_free(process_var);

        /*  Legacy App */
        DbusTestDbusMockObject* ljobobj =
            dbus_test_dbus_mock_get_object(mock, "/com/test/application_legacy", UPSTART_JOB, NULL);

        dbus_test_dbus_mock_object_add_method(mock, ljobobj, "Start", G_VARIANT_TYPE("(asb)"), NULL, "", NULL);

        dbus_test_dbus_mock_object_add_method(mock, ljobobj, "Stop", G_VARIANT_TYPE("(asb)"), NULL, "", NULL);

        dbus_test_dbus_mock_object_add_method(mock, ljobobj, "GetAllInstances", NULL, G_VARIANT_TYPE("ao"),
                                              "ret = [ dbus.ObjectPath('/com/test/legacy_app_instance') ]", NULL);

        DbusTestDbusMockObject* linstobj = dbus_test_dbus_mock_get_object(mock, "/com/test/legacy_app_instance",
                                                                          UPSTART_INSTANCE, NULL);
        dbus_test_dbus_mock_object_add_property(mock, linstobj, "name", G_VARIANT_TYPE_STRING,
                                                g_variant_new_string("multiple-2342345"), NULL);
        dbus_test_dbus_mock_object_add_property(mock, linstobj, "processes", G_VARIANT_TYPE("a(si)"),
                                                g_variant_new_parsed("[('main', 5678)]"), NULL);

        /*  Untrusted Helper */
        DbusTestDbusMockObject* uhelperobj =
            dbus_test_dbus_mock_get_object(mock, UNTRUSTED_HELPER_PATH, UPSTART_JOB, NULL);

        dbus_test_dbus_mock_object_add_method(mock, uhelperobj, "Start", G_VARIANT_TYPE("(asb)"), NULL,
                            "import os\n"
                            "import sys\n"
                            "import subprocess\n"
                            "target = open(\"/tmp/testHelper\", 'w')\n"
                            "exec_app=\"\"\n"
                            "for item in args[0]:\n"
                            "    keyVal = str(item)\n"
                            "    keyVal = keyVal.split(\"=\")\n"
                            "    if len(keyVal) == 2:\n"
                            "        os.environ[keyVal[0]] = keyVal[1]\n"
                            "        if keyVal[0] == \"APP_URIS\":\n"
                            "            exec_app = keyVal[1].replace(\"'\", '')\n"
                            "            target.write(exec_app)\n"
                            "            params = exec_app.split()\n"
                            "            if len(params) > 1:\n"
                            "                os.chdir(params[1])\n"
                            "                proc = subprocess.Popen(params[0], shell=True, stdout=subprocess.PIPE)\n"
                            "target.close\n"
                            , NULL);

        dbus_test_dbus_mock_object_add_method(mock, uhelperobj, "Stop", G_VARIANT_TYPE("(asb)"), NULL, "", NULL);

        dbus_test_dbus_mock_object_add_method(mock, uhelperobj, "GetAllInstances", NULL, G_VARIANT_TYPE("ao"),
                                              "ret = [ dbus.ObjectPath('/com/test/untrusted/helper/instance'), "
                                              "dbus.ObjectPath('/com/test/untrusted/helper/multi_instance') ]",
                                              NULL);

        DbusTestDbusMockObject* uhelperinstance = dbus_test_dbus_mock_get_object(
            mock, "/com/test/untrusted/helper/instance", UPSTART_INSTANCE, NULL);
        dbus_test_dbus_mock_object_add_property(mock, uhelperinstance, "name", G_VARIANT_TYPE_STRING,
                                                g_variant_new_string("untrusted-type::com.foo_bar_43.23.12"), NULL);

        DbusTestDbusMockObject* unhelpermulti = dbus_test_dbus_mock_get_object(
            mock, "/com/test/untrusted/helper/multi_instance", UPSTART_INSTANCE, NULL);
        dbus_test_dbus_mock_object_add_property(
            mock, unhelpermulti, "name", G_VARIANT_TYPE_STRING,
            g_variant_new_string("backup-helper:24034582324132:com.bar_foo_8432.13.1"), NULL);

        /* Create the cgroup manager mock */
        cgmock = dbus_test_dbus_mock_new("org.test.cgmock");
        g_setenv("UBUNTU_APP_LAUNCH_CG_MANAGER_NAME", "org.test.cgmock", TRUE);

        DbusTestDbusMockObject* cgobject = dbus_test_dbus_mock_get_object(cgmock, "/org/linuxcontainers/cgmanager",
                                                                          "org.linuxcontainers.cgmanager0_0", NULL);
        dbus_test_dbus_mock_object_add_method(cgmock, cgobject, "GetTasksRecursive", G_VARIANT_TYPE("(ss)"),
                                              G_VARIANT_TYPE("ai"), "ret = [100, 200, 300]", NULL);

        /* Put it together */
        dbus_test_service_add_task(service, DBUS_TEST_TASK(mock));
        dbus_test_service_add_task(service, DBUS_TEST_TASK(cgmock));
    }

    virtual void TearDown() override
    {
        registry.reset();

        ubuntu_app_launch_observer_delete_app_focus(focus_cb, this);
        ubuntu_app_launch_observer_delete_app_resume(resume_cb, this);

        g_clear_object(&mock);
        g_clear_object(&cgmock);
        g_clear_object(&service);

        g_object_unref(bus);

        unsigned int cleartry = 0;
        while (bus != NULL && cleartry < 100)
        {
            pause(100);
            cleartry++;
        }

        // if the test failed, keep the artifacts so devs can examine them
        QDir data_home_dir(CMAKE_SOURCE_DIR "/libertine-home");
        const auto passed = ::testing::UnitTest::GetInstance()->current_test_info()->result()->Passed();
        if (passed)
            data_home_dir.removeRecursively();
        else
            qDebug("test failed; leaving '%s'", data_home_dir.path().toUtf8().constData());

        ASSERT_EQ(nullptr, bus);

        // let's leave things clean
        EXPECT_TRUE(removeHelperMarkBeforeStarting());
    }

    bool startKeeperClient()
    {
        qDebug("starting keeper client '%s'", KEEPER_CLIENT_BIN);
        keeper_client.start(KEEPER_CLIENT_BIN, QStringList());

        if (!keeper_client.waitForStarted())
            return false;

        return true;
    }

    bool checkStorageFrameworkFiles(QStringList const & sourceDirs, bool compression=false)
    {
        QStringList dirs = sourceDirs;

        while (dirs.size() > 0)
        {
            auto dir = dirs.takeLast();
            QString lastFile = getLastStorageFrameworkFile();
            if (lastFile.isEmpty())
            {
                qWarning() << "Did not found enough storage framework files";
                return false;
            }
            if (!compareTarContent (lastFile, dir, compression))
            {
                return false;
            }
            // remove the last file, so next iteration the last one is different
            QFile::remove(lastFile);
        }
        return true;
    }

    bool checkLastStorageFrameworkFile (QString const & sourceDir, bool compression=false)
    {
        QString lastFile = getLastStorageFrameworkFile();
        if (lastFile.isEmpty())
        {
            qWarning() << "Last file from storage framework is empty";
            return false;
        }
        return compareTarContent (lastFile, sourceDir, compression);
    }

    bool compareTarContent (QString const & tarPath, QString const & sourceDir, bool compression)
    {
        QTemporaryDir tempDir;

        qDebug() << "Comparing tar content for dir: " << sourceDir << " with tar: " << tarPath;

        QFileInfo checkFile(tarPath);
        if (!checkFile.exists())
        {
            qWarning() << "File: " << tarPath << " does not exist";
            return false;
        }
        if (!checkFile.isFile())
        {
            qWarning() << "Item: " << tarPath << " is not a file";
            return false;
        }
        if (!tempDir.isValid())
        {
            qWarning() << "Temporary directory: " << tempDir.path() << " is not valid";
            return false;
        }

        if( !extractTarContents(tarPath, tempDir.path()))
        {
            return false;
        }
        return FileUtils::compareDirectories(sourceDir, tempDir.path());
    }

    bool extractTarContents(QString const & tarPath, QString const & destination, bool compression=false)
    {
        QProcess tarProcess;
        QString tarParams = compression ? QString("-xzvf") : QString("-xvf");
        qDebug() << "Starting the process...";
        tarProcess.start("tar", QStringList()
                                        << "-C"
                                        << destination
                                        << tarParams
                                        << tarPath);
        if (!tarProcess.waitForStarted())
        {
            qWarning() << "Error starting tar process: " << tarProcess.errorString();
            return false;
        }

        if (!tarProcess.waitForFinished())
        {
            qWarning() << "Error waiting for tar process: " << tarProcess.errorString();
            return false;
        }

        if (tarProcess.exitCode() != 0)
        {
            qWarning() << "Process error: " << tarProcess.readAllStandardError();
        }
        return tarProcess.exitCode() == 0;
    }

    QString getLastStorageFrameworkFile()
    {
        // search the storage framework file that the helper created
        auto data_home = qgetenv("XDG_DATA_HOME");
        if (data_home == "")
        {
            qWarning() << "ERROR: XDG_DATA_HOME is not defined";
            return QString();
        }
        qDebug() << "XDG_DATA_HOME is: " << data_home;
        QString storage_framework_dir_path = QString("%1%2storage-framework").arg(QString(data_home)).arg(QDir::separator());
        QDir storage_framework_dir(storage_framework_dir_path);
        if (!storage_framework_dir.exists())
        {
            qWarning() << "ERROR: Storage framework directory: [" << storage_framework_dir_path << "] does not exist.";
            return QString();
        }

        QStringList sortedFiles;
        QFileInfoList files = storage_framework_dir.entryInfoList();
        for (int i = 0; i < files.size(); ++i)
        {
            QFileInfo file = files[i];
            if (file.isFile())
            {
                sortedFiles << files[i].absoluteFilePath();
            }
        }

        // we detect the last file by name.
        // the file creation time had not enough precision
        sortedFiles.sort();
        if (sortedFiles.isEmpty())
        {
            qWarning() << "ERROR: last file in the storage-framework directory was not found";
            return QString();
        }
        return sortedFiles.last();
    }

    bool checkStorageFrameworkContent(QString const & content)
    {
        QString lastFile = getLastStorageFrameworkFile();
        if (lastFile.isEmpty())
        {
            qWarning() << "Last file from the storage framework was not found";
            return false;
        }
        QFile storage_framework_file(lastFile);
        if(!storage_framework_file.open(QFile::ReadOnly))
        {
            qWarning() << "ERROR: opening file: " << lastFile;
            return false;
        }

        QString file_content = storage_framework_file.readAll();

        return file_content == content;
    }

    bool removeHelperMarkBeforeStarting()
    {
        QFile helper_mark(SIMPLE_HELPER_MARK_FILE_PATH);
        if (helper_mark.exists())
        {
            return helper_mark.remove();
        }
        return true;
    }

    bool waitUntilHelperFinishes(QString const & app_id, int maxTimeout = 15000, int times = 1)
    {
        // TODO create a new mock for upstart that controls the lifecycle of the
        // helper process so we can do this in a cleaner way.
        QFile helper_mark(SIMPLE_HELPER_MARK_FILE_PATH);
        QElapsedTimer timer;
        timer.start();
        auto times_to_wait = times;
        bool finished = false;
        while (!timer.hasExpired(maxTimeout) && !finished)
        {
            if (helper_mark.exists())
            {
                if (--times_to_wait)
                {
                    helper_mark.remove();
                    timer.restart();
                    qDebug() << "HELPER FINISHED, WAITING FOR " << times_to_wait << " MORE";
                }
                else
                {
                    qDebug() << "ALL HELPERS FINISHED";
                    finished = true;
                }
                sendUpstartHelperTermination(app_id);
            }
        }
        return finished;
    }

    void sendUpstartHelperTermination(QString const &app_id)
    {
        // send the upstart signal so keeper-service is aware of the helper termination
        DbusTestDbusMockObject* objUpstart =
            dbus_test_dbus_mock_get_object(mock, UPSTART_PATH, UPSTART_INTERFACE, NULL);

        QString eventInfoStr = QString("('stopped', ['JOB=untrusted-helper', 'INSTANCE=backup-helper::%1'])").arg(app_id.toStdString().c_str());
        dbus_test_dbus_mock_object_emit_signal(
            mock, objUpstart, "EventEmitted", G_VARIANT_TYPE("(sas)"),
            g_variant_new_parsed(
                    eventInfoStr.toLocal8Bit().data()),
            NULL);
        g_usleep(100000);
        while (g_main_pending())
        {
            g_main_iteration(TRUE);
        }
    }

    QString getUUIDforXdgFolderPath(QString const &path, QVariantDictMap const & choices) const
    {
        for(auto iter = choices.begin(); iter != choices.end(); ++iter)
        {
            const auto& values = iter.value();
            auto iter_values = values.find("path");
            if (iter_values != values.end())
            {
                if (iter_values.value().toString() == path)
                {
                    // got it
                    return iter.key();
                }
            }
        }

        return QString();
    }

    GVariant* find_env(GVariant* env_array, const gchar* var)
    {
        GVariant* retval = nullptr;

        for (int i=0, n=g_variant_n_children(env_array); i<n; i++)
        {
            GVariant* child = g_variant_get_child_value(env_array, i);
            const gchar* envvar = g_variant_get_string(child, nullptr);

            if (g_str_has_prefix(envvar, var))
            {
                if (retval != nullptr)
                {
                    g_warning("Found the env var more than once!");
                    g_variant_unref(retval);
                    return nullptr;
                }

                retval = child;
            }
            else
            {
                g_variant_unref(child);
            }
        }

        if (!retval)
        {
            gchar* envstr = g_variant_print(env_array, FALSE);
            g_warning("Unable to find '%s' in '%s'", var, envstr);
            g_free(envstr);
        }

        return retval;
    }

    std::string get_env(GVariant* env_array, const gchar* key)
    {
        std::string value;

        GVariant* variant = find_env(env_array, key);
        if (variant != nullptr)
        {
            const char* cstr = g_variant_get_string(variant, nullptr);
            if (cstr != nullptr)
            {
                cstr = strchr(cstr, '=');
                if (cstr != nullptr)
                    value = cstr + 1;
            }

            g_clear_pointer(&variant, g_variant_unref);
        }

        return value;
    }

    bool have_env(GVariant* env_array, const gchar* key)
    {
        GVariant* variant = find_env(env_array, key);
        bool found = variant != nullptr;
        g_clear_pointer(&variant, g_variant_unref);
        return found;
    }

    void pause(guint time = 0)
    {
        if (time > 0)
        {
            GMainLoop* mainloop = g_main_loop_new(NULL, FALSE);

            g_timeout_add(time,
                          [](gpointer pmainloop) -> gboolean
                          {
                              g_main_loop_quit(static_cast<GMainLoop*>(pmainloop));
                              return G_SOURCE_REMOVE;
                          },
                          mainloop);

            g_main_loop_run(mainloop);

            g_main_loop_unref(mainloop);
        }

        while (g_main_pending())
        {
            g_main_iteration(TRUE);
        }
    }
};

#define EXPECT_ENV(expected, envvars, key) EXPECT_EQ(expected, get_env(envvars, key)) << "for key " << key
#define ASSERT_ENV(expected, envvars, key) ASSERT_EQ(expected, get_env(envvars, key)) << "for key " << key

TEST_F(TestHelpers, StartHelper)
{
    // starts the services, including keeper-service
    startTasks();

    DbusTestDbusMockObject* obj =
        dbus_test_dbus_mock_get_object(mock, UNTRUSTED_HELPER_PATH, UPSTART_JOB, NULL);

    BackupHelper helper("com.test.multiple_first_1.2.3");
    helper.set_bin_path(DEKKO_HELPER_BIN);
    helper.set_main_dir_path(DEKKO_HELPER_DIR);

    QSignalSpy spy(&helper, &BackupHelper::state_changed);

    helper.start();

    guint len = 0;
    auto calls = dbus_test_dbus_mock_object_get_method_calls(mock, obj, "Start", &len, NULL);
    EXPECT_NE(nullptr, calls);
    EXPECT_EQ(1, len);

    auto env = g_variant_get_child_value(calls->params, 0);
    EXPECT_ENV("com.test.multiple_first_1.2.3", env, "APP_ID");

    QString appUrisStr = QString("'%1' '%2'").arg(DEKKO_HELPER_BIN).arg(DEKKO_HELPER_DIR);
    EXPECT_ENV(appUrisStr.toLocal8Bit().data(), env, "APP_URIS");
    EXPECT_ENV("backup-helper", env, "HELPER_TYPE");
    EXPECT_TRUE(have_env(env, "INSTANCE_ID"));
    g_variant_unref(env);

    DbusTestDbusMockObject* objUpstart =
        dbus_test_dbus_mock_get_object(mock, UPSTART_PATH, UPSTART_INTERFACE, NULL);

    /* Basic start */
    dbus_test_dbus_mock_object_emit_signal(
        mock, objUpstart, "EventEmitted", G_VARIANT_TYPE("(sas)"),
        g_variant_new_parsed("('started', ['JOB=untrusted-helper', 'INSTANCE=backup-helper::com.test.multiple_first_1.2.3'])"),
        NULL);

    // we set a timeout of 5 seconds waiting for the signal to be emitted,
    // which should never be reached
    ASSERT_TRUE(spy.wait(5000));

    // check that we've got exactly one signal
    ASSERT_EQ(spy.count(), 1);

    g_usleep(100000);
    while (g_main_pending())
    {
        g_main_iteration(TRUE);
    }

    helper.stop();
}

TEST_F(TestHelpers, StopHelper)
{
    // starts the services, including keeper-service
    startTasks();

    DbusTestDbusMockObject* obj =
        dbus_test_dbus_mock_get_object(mock, UNTRUSTED_HELPER_PATH, UPSTART_JOB, NULL);

    BackupHelper helper("com.bar_foo_8432.13.1");
    QSignalSpy spy(&helper, &BackupHelper::state_changed);

    helper.stop();
    ASSERT_EQ(dbus_test_dbus_mock_object_check_method_call(mock, obj, "Stop", NULL, NULL), 1);

    guint len = 0;
    auto calls = dbus_test_dbus_mock_object_get_method_calls(mock, obj, "Stop", &len, NULL);
    EXPECT_NE(nullptr, calls);
    EXPECT_EQ(1, len);

    EXPECT_STREQ("Stop", calls->name);
    EXPECT_EQ(2, g_variant_n_children(calls->params));

    auto block = g_variant_get_child_value(calls->params, 1);
    EXPECT_TRUE(g_variant_get_boolean(block));
    g_variant_unref(block);

    auto env = g_variant_get_child_value(calls->params, 0);
    EXPECT_ENV("com.bar_foo_8432.13.1", env, "APP_ID");
    EXPECT_ENV("backup-helper", env, "HELPER_TYPE");
    EXPECT_ENV("24034582324132", env, "INSTANCE_ID");
    g_variant_unref(env);

    ASSERT_TRUE(dbus_test_dbus_mock_object_clear_method_calls(mock, obj, NULL));

    DbusTestDbusMockObject* objUpstart =
        dbus_test_dbus_mock_get_object(mock, UPSTART_PATH, UPSTART_INTERFACE, NULL);

    dbus_test_dbus_mock_object_emit_signal(
        mock, objUpstart, "EventEmitted", G_VARIANT_TYPE("(sas)"),
        g_variant_new_parsed(
            "('stopped', ['JOB=untrusted-helper', 'INSTANCE=backup-helper::com.bar_foo_8432.13.1'])"),
        NULL);

    // we set a timeout of 5 seconds waiting for the signal to be emitted,
    // which should never be reached
    ASSERT_TRUE(spy.wait(5000));

    // check that we've got exactly one signal
    ASSERT_EQ(spy.count(), 1);

    g_usleep(100000);
    while (g_main_pending())
    {
        g_main_iteration(TRUE);
    }
}

typedef struct
{
    unsigned int count;
    const gchar* appid;
    const gchar* type;
    const gchar* instance;
} helper_observer_data_t;

static void helper_observer_cb(const gchar* appid, const gchar* instance, const gchar* type, gpointer user_data)
{
    helper_observer_data_t* data = (helper_observer_data_t*)user_data;

    if (g_strcmp0(data->appid, appid) == 0 && g_strcmp0(data->type, type) == 0 &&
        g_strcmp0(data->instance, instance) == 0)
    {
        data->count++;
    }
}

TEST_F(TestHelpers, StartStopHelperObserver)
{
    // starts the services, including keeper-service
    startTasks();

    helper_observer_data_t start_data = {
        .count = 0, .appid = "com.foo_foo_1.2.3", .type = "my-type-is-scorpio", .instance = nullptr};
    helper_observer_data_t stop_data = {
        .count = 0, .appid = "com.bar_bar_44.32", .type = "my-type-is-libra", .instance = "1234"};

    ASSERT_TRUE(ubuntu_app_launch_observer_add_helper_started(helper_observer_cb, "my-type-is-scorpio", &start_data));
    ASSERT_TRUE(ubuntu_app_launch_observer_add_helper_stop(helper_observer_cb, "my-type-is-libra", &stop_data));

    DbusTestDbusMockObject* obj =
        dbus_test_dbus_mock_get_object(mock, UPSTART_PATH, UPSTART_INTERFACE, NULL);

    /* Basic start */
    dbus_test_dbus_mock_object_emit_signal(
        mock, obj, "EventEmitted", G_VARIANT_TYPE("(sas)"),
        g_variant_new_parsed("('started', ['JOB=untrusted-helper', 'INSTANCE=my-type-is-scorpio::com.foo_foo_1.2.3'])"),
        NULL);

    g_usleep(100000);
    while (g_main_pending())
    {
        g_main_iteration(TRUE);
    }

    ASSERT_EQ(start_data.count, 1);

    /* Basic stop */
    dbus_test_dbus_mock_object_emit_signal(
        mock, obj, "EventEmitted", G_VARIANT_TYPE("(sas)"),
        g_variant_new_parsed(
            "('stopped', ['JOB=untrusted-helper', 'INSTANCE=my-type-is-libra:1234:com.bar_bar_44.32'])"),
        NULL);

    g_usleep(100000);
    while (g_main_pending())
    {
        g_main_iteration(TRUE);
    }

    ASSERT_EQ(stop_data.count, 1);


    /* Remove */
    ASSERT_TRUE(
        ubuntu_app_launch_observer_delete_helper_started(helper_observer_cb, "my-type-is-scorpio", &start_data));
    ASSERT_TRUE(ubuntu_app_launch_observer_delete_helper_stop(helper_observer_cb, "my-type-is-libra", &stop_data));
}

TEST_F(TestHelpers, StartFullTest)
{
    g_setenv("KEEPER_TEST_HELPER", TEST_SIMPLE_HELPER_SH, TRUE);

    XdgUserDirsSandbox tmp_dir;

    // starts the services, including keeper-service
    startTasks();

    QDBusConnection connection = QDBusConnection::sessionBus();
    QScopedPointer<DBusInterfaceKeeperUser> user_iface(new DBusInterfaceKeeperUser(
                                                            DBusTypes::KEEPER_SERVICE,
                                                            DBusTypes::KEEPER_USER_PATH,
                                                            connection
                                                        ) );

    ASSERT_TRUE(user_iface->isValid()) << qPrintable(QDBusConnection::sessionBus().lastError().message());

    // ask for a list of backup choices
    QDBusReply<QVariantDictMap> choices = user_iface->call("GetBackupChoices");
    EXPECT_TRUE(choices.isValid()) << qPrintable(choices.error().message());

    QString user_option = "XDG_MUSIC_DIR";

    auto user_dir = qgetenv(user_option.toLatin1().data());
    ASSERT_FALSE(user_dir.isEmpty());
    qDebug() << "USER DIR: " << user_dir;

    // fill something in the music dir
    ASSERT_TRUE(FileUtils::fillTemporaryDirectory(user_dir, qrand() % 1000));

    // search for the user folder uuid
    auto user_folder_uuid = getUUIDforXdgFolderPath(user_dir, choices.value());
    ASSERT_FALSE(user_folder_uuid.isEmpty());
    qDebug() << "User folder UUID is: " << user_folder_uuid;

    QString user_option_2 = "XDG_VIDEOS_DIR";

    auto user_dir_2 = qgetenv(user_option_2.toLatin1().data());
    ASSERT_FALSE(user_dir_2.isEmpty());
    qDebug() << "USER DIR 2: " << user_dir_2;

    // fill something in the music dir
    ASSERT_TRUE(FileUtils::fillTemporaryDirectory(user_dir_2, qrand() % 1000));

    // search for the user folder uuid
    auto user_folder_uuid_2 = getUUIDforXdgFolderPath(user_dir_2, choices.value());
    ASSERT_FALSE(user_folder_uuid_2.isEmpty());
    qDebug() << "User folder 2 UUID is: " << user_folder_uuid_2;

    // Now we know the music folder uuid, let's start the backup for it.
    QDBusReply<void> backup_reply = user_iface->call("StartBackup", QStringList{user_folder_uuid, user_folder_uuid_2});
    ASSERT_TRUE(backup_reply.isValid()) << qPrintable(QDBusConnection::sessionBus().lastError().message());

    // Wait until the helper finishes
    EXPECT_TRUE(waitUntilHelperFinishes(DEKKO_APP_ID, 15000, 2));

    // check that the content of the file is the expected
    EXPECT_TRUE(checkStorageFrameworkFiles(QStringList{user_dir, user_dir_2}));
    // let's leave things clean
    EXPECT_TRUE(removeHelperMarkBeforeStarting());

    g_unsetenv("KEEPER_TEST_HELPER");
}

TEST_F(TestHelpers, Inactivity)
{
    // starts the services, including keeper-service
    startTasks();

    DbusTestDbusMockObject* obj =
        dbus_test_dbus_mock_get_object(mock, UNTRUSTED_HELPER_PATH, UPSTART_JOB, NULL);

    BackupHelper helper("com.bar_foo_8432.13.1");

    helper.start();

    DbusTestDbusMockObject* objUpstart =
        dbus_test_dbus_mock_get_object(mock, UPSTART_PATH, UPSTART_INTERFACE, NULL);

    /* Basic start */
    dbus_test_dbus_mock_object_emit_signal(
        mock, objUpstart, "EventEmitted", G_VARIANT_TYPE("(sas)"),
        g_variant_new_parsed("('started', ['JOB=untrusted-helper', 'INSTANCE=backup-helper::com.bar_foo_8432.13.1'])"),
        NULL);

    QElapsedTimer timer;
    timer.start();
    int nb_stop_calls = 0;
    // we wait 1 second more compared to the inactivity time...
    while(!timer.hasExpired(BackupHelper::MAX_INACTIVITY_TIME + 1000) && (nb_stop_calls == 0))
    {
        nb_stop_calls = dbus_test_dbus_mock_object_check_method_call(mock, obj, "Stop", NULL, NULL);
        QCoreApplication::processEvents();
    }

    EXPECT_EQ(nb_stop_calls, 1);

    dbus_test_dbus_mock_object_emit_signal(
            mock, objUpstart, "EventEmitted", G_VARIANT_TYPE("(sas)"),
            g_variant_new_parsed(
                "('stopped', ['JOB=untrusted-helper', 'INSTANCE=backup-helper::com.bar_foo_8432.13.1'])"),
            NULL);

    g_usleep(100000);
    while (g_main_pending())
    {
        g_main_iteration(TRUE);
    }
}
