package org.ccnx.ccn.utils;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.logging.Level;

import org.ccnx.ccn.CCNHandle;
import org.ccnx.ccn.config.ConfigurationException;
import org.ccnx.ccn.impl.support.Log;
import org.ccnx.ccn.io.CCNFileInputStream;
import org.ccnx.ccn.io.CCNInputStream;
import org.ccnx.ccn.protocol.ContentName;
import org.ccnx.ccn.protocol.MalformedContentNameStringException;

public class ccngetjar {
    public static Integer timeout = null;
    static int startArg = 0;
    public static void main(String[] args) {
        try {
            int readsize = 1024; // make an argument for testing...
            // If we get one file name, put as the specific name given.
            // If we get more than one, put underneath the first as parent.
            // Ideally want to use newVersion to get latest version. Start
            // with random version.
            ContentName argName = ContentName.fromURI(args[startArg]);
            
            CCNHandle handle = CCNHandle.open();

            File theFile = new File(args[startArg]);
            if (theFile.exists()) {
                System.out.println("Overwriting file: " + args[startArg]);
            }
            FileOutputStream output = new FileOutputStream(theFile);
            
            long starttime = System.currentTimeMillis();
            CCNInputStream input;
            /*if (unversioned)
                input = new CCNInputStream(argName, handle);*/
            //else
            input = new CCNFileInputStream(argName, handle);
            if (timeout != null) {
                input.setTimeout(timeout); 
            }
            byte [] buffer = new byte[readsize];
            
            int readcount = 0;
            long readtotal = 0;
            while ((readcount = input.read(buffer)) != -1){
                readtotal += readcount;
                output.write(buffer, 0, readcount);
                output.flush();
            }
            System.out.println("ccngetfile took: "+(System.currentTimeMillis() - starttime)+"ms");
            //System.out.println("Retrieved content " + args[1] + " got " + readtotal + " bytes.");
            System.exit(0);

        } catch (ConfigurationException e) {
            System.out.println("Configuration exception in ccngetfile: " + e.getMessage());
            e.printStackTrace();
        } catch (MalformedContentNameStringException e) {
            System.out.println("Malformed name: " + args[0] + " " + e.getMessage());
            e.printStackTrace();
        } catch (IOException e) {
            System.out.println("Cannot write file or read content. " + e.getMessage());
            e.printStackTrace();
        }
        System.exit(1);
    }
}

