package org.ccnx.ccn.apps.ccnfileproxy;

import java.lang.reflect.*;
import java.io.*;

/* Class to dynamically load java JAR files during runtime */
public class DynamicLoader {
    private String smil = "";

    public String getSmil() {
        return smil;
    }

	public DynamicLoader(String file, String service) throws Exception {
		// ClasspathHacker JAR loader class code from
		// http://stackoverflow.com/questions/60764/how-should-i-load-jars-dynamically-at-runtime
        File f = new File(".");
		ClasspathHacker.addFile(service + ".jar");
		System.out.printf ("Loading: %s.jar\n", service);
		Class<?> p = ClassLoader.getSystemClassLoader().loadClass(service);
		Object processor = p.newInstance();
		// Following code is adapted from
		// http://java.sun.com/developer/technicalArticles/ALT/Reflection/
		Class[] parameterTypes = new Class[] { String.class };
	    Method concatMethod;
	    Object[] arguments = new Object[] { file };
	    String result = "";
		try {
	      concatMethod = p.getMethod("run_" + service, parameterTypes);
	      smil = (String) concatMethod.invoke(processor, arguments);
	    } catch (Exception e) {
	      e.printStackTrace();
	    }
	}
}    
